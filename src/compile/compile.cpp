#include "ossie/compile.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "ossie/catalog.hpp"

namespace duckdb {
namespace ossie {

namespace {

//! Aliasing each dataset to its model name keeps emitted SQL readable, but assumes a dataset
//! appears at most once per query. Joining one twice would need aliases keyed by join path.
const string &AliasFor(const Dataset &dataset) {
	return dataset.name;
}

//! Points the column refs inside an inlined field expression at the bound alias.
void QualifyColumns(unique_ptr<ParsedExpression> &expr, const string &alias) {
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		expr = make_uniq<ColumnRefExpression>(colref.column_names.back(), alias);
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { QualifyColumns(child, alias); });
}

//! Replaces `dataset.field` with the field's expression. Field names are model-level and need not
//! exist on the table: `customer_full_name` is really `c_first_name || ' ' || c_last_name`.
void InlineFields(const Model &model, unique_ptr<ParsedExpression> &expr) {
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (colref.column_names.size() == 2) {
			auto dataset = model.FindDataset(colref.column_names[0]);
			auto field = dataset->FindField(colref.column_names[1]);
			auto replacement = field->expression.tree->Copy();
			QualifyColumns(replacement, AliasFor(*dataset));
			expr = std::move(replacement);
			return;
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { InlineFields(model, child); });
}

void CollectDatasets(const ParsedExpression &expr, case_insensitive_set_t &datasets) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		if (colref.column_names.size() == 2) {
			datasets.insert(colref.column_names[0]);
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { CollectDatasets(child, datasets); });
}

vector<string> MetricNames(const Model &model) {
	vector<string> names;
	for (auto &metric : model.metrics) {
		names.push_back(metric.name);
	}
	return names;
}

vector<string> DimensionNames(const Model &model) {
	vector<string> names;
	for (auto &dataset : model.datasets) {
		for (auto &field : dataset.fields) {
			names.push_back(dataset.name + "." + field.name);
		}
	}
	return names;
}

unique_ptr<TableRef> BindTable(const Dataset &dataset) {
	auto table_ref = make_uniq<BaseTableRef>();
	SplitSource(dataset.source_bound, table_ref->catalog_name, table_ref->schema_name, table_ref->table_name);
	table_ref->alias = AliasFor(dataset);
	return std::move(table_ref);
}

//! A dimension request e.g. "item.i_brand".
struct Dimension {
	const Dataset *dataset;
	unique_ptr<ParsedExpression> expr;
	string alias;
};

Dimension ResolveDimension(const Model &model, const string &request) {
	vector<unique_ptr<ParsedExpression>> parsed;
	try {
		parsed = Parser::ParseExpressionList(request);
	} catch (const ParserException &) {
		throw InvalidInputException("ossie_compile: dimension \"%s\" is not a valid name", request);
	}
	if (parsed.size() != 1 || parsed[0]->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		throw InvalidInputException("ossie_compile: dimension \"%s\" must be a dataset.field name", request);
	}
	auto &colref = parsed[0]->Cast<ColumnRefExpression>();
	if (colref.column_names.size() != 2) {
		throw InvalidInputException("ossie_compile: dimension \"%s\" must be qualified as dataset.field", request);
	}

	auto dataset = model.FindDataset(colref.column_names[0]);
	if (!dataset) {
		throw InvalidInputException("ossie_compile: dimension \"%s\" names dataset \"%s\", which the model does not "
		                            "declare",
		                            request, colref.column_names[0]);
	}
	if (!dataset->FindField(colref.column_names[1])) {
		throw InvalidInputException("ossie_compile: dataset \"%s\" has no field \"%s\".%s", dataset->name,
		                            colref.column_names[1],
		                            StringUtil::CandidatesErrorMessage(DimensionNames(model), request, "Did you mean"));
	}

	Dimension result;
	result.dataset = dataset;
	result.expr = std::move(parsed[0]);
	InlineFields(model, result.expr);
	result.alias = request;
	return result;
}

struct PlannedJoin {
	const Relationship *relationship;
	const Dataset *dataset;
};

//! Executes BFS through the relationship graph outward from the metric's dataset, returning the joins needed to
//! reach every required dataset. Refuses ambiguous joins (when multiple routes to join two datasets exist).
vector<PlannedJoin> PlanJoins(const Model &model, const Dataset &root, const case_insensitive_set_t &required) {
	case_insensitive_map_t<const Relationship *> parent_edge;
	case_insensitive_map_t<string> parent;
	case_insensitive_set_t visited {root.name};
	vector<string> queue {root.name};

	for (idx_t head = 0; head < queue.size(); head++) {
		auto current = queue[head];
		for (auto &relationship : model.relationships) {
			string other;
			if (StringUtil::CIEquals(relationship.from_dataset, current)) {
				other = relationship.to_dataset;
			} else if (StringUtil::CIEquals(relationship.to_dataset, current)) {
				other = relationship.from_dataset;
			} else {
				continue;
			}

			if (visited.find(other) == visited.end()) {
				visited.insert(other);
				parent_edge[other] = &relationship;
				parent[other] = current;
				queue.push_back(other);
				continue;
			}
			// A second edge into an already-reached dataset means two routes exist, and they can
			// produce different numbers. We refuse this ambiguous case.
			auto reached_by = parent_edge.find(other);
			auto came_by = parent_edge.find(current);
			bool is_tree_edge = (reached_by != parent_edge.end() && reached_by->second == &relationship) ||
			                    (came_by != parent_edge.end() && came_by->second == &relationship);
			if (!is_tree_edge && required.find(other) != required.end()) {
				throw InvalidInputException("ossie_compile: more than one join path connects \"%s\" to \"%s\"; "
				                            "the model must disambiguate before this can be answered",
				                            root.name, other);
			}
		}
	}

	// Collect the tree edges on the path from each required dataset back to the root.
	vector<PlannedJoin> joins;
	case_insensitive_set_t added;
	for (auto &name : required) {
		if (visited.find(name) == visited.end()) {
			throw InvalidInputException("ossie_compile: no relationship connects \"%s\" to \"%s\"", root.name, name);
		}
		vector<string> path;
		for (auto node = name; !StringUtil::CIEquals(node, root.name); node = parent[node]) {
			path.push_back(node);
		}
		// Root-first, so every join attaches to a dataset already in the tree.
		for (auto entry = path.rbegin(); entry != path.rend(); ++entry) {
			if (added.insert(*entry).second) {
				joins.push_back({parent_edge[*entry], model.FindDataset(*entry)});
			}
		}
	}
	return joins;
}

unique_ptr<ParsedExpression> JoinCondition(const Relationship &relationship) {
	unique_ptr<ParsedExpression> condition;
	for (idx_t i = 0; i < relationship.from_columns.size(); i++) {
		auto left = make_uniq<ColumnRefExpression>(relationship.from_columns[i], relationship.from_dataset);
		auto right = make_uniq<ColumnRefExpression>(relationship.to_columns[i], relationship.to_dataset);
		auto equality =
		    make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, std::move(left), std::move(right));
		if (!condition) {
			condition = std::move(equality);
		} else {
			condition = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(condition),
			                                             std::move(equality));
		}
	}
	return condition;
}

} // namespace

string CompileToSQL(const Model &model, const vector<string> &metrics, const vector<string> &dimensions,
                    const vector<string> &filters) {
	if (metrics.empty()) {
		throw InvalidInputException("ossie_compile: at least one metric is required");
	}
	if (!filters.empty()) {
		throw InvalidInputException("ossie_compile: filters are not supported yet");
	}

	auto select = make_uniq<SelectNode>();
	case_insensitive_set_t metric_datasets;
	vector<unique_ptr<ParsedExpression>> metric_expressions;

	for (auto &metric_name : metrics) {
		auto metric = model.FindMetric(metric_name);
		if (!metric) {
			throw InvalidInputException(
			    "ossie_compile: model \"%s\" has no metric named \"%s\".%s", model.name, metric_name,
			    StringUtil::CandidatesErrorMessage(MetricNames(model), metric_name, "Did you mean"));
		}
		CollectDatasets(*metric->expression.tree, metric_datasets);

		auto expr = metric->expression.tree->Copy();
		InlineFields(model, expr);
		expr->SetAlias(metric->name);
		metric_expressions.push_back(std::move(expr));
	}

	// Metrics must share one dataset: aggregating across a join can multiply rows into the result,
	// which we don't handle yet
	if (metric_datasets.size() != 1) {
		throw InvalidInputException("ossie_compile: these metrics aggregate over %s datasets, which needs each "
		                            "aggregate computed at its own grain",
		                            to_string(metric_datasets.size()));
	}
	auto &root = *model.FindDataset(*metric_datasets.begin());

	// Dimensions are projected and grouped; they may pull in datasets the metrics do not touch.
	case_insensitive_set_t required;
	for (auto &request : dimensions) {
		auto dimension = ResolveDimension(model, request);
		if (!StringUtil::CIEquals(dimension.dataset->name, root.name)) {
			required.insert(dimension.dataset->name);
		}
		auto index = select->select_list.size();
		dimension.expr->SetAlias(dimension.alias);
		select->select_list.push_back(std::move(dimension.expr));
		select->groups.group_expressions.push_back(select->select_list[index]->Copy());
	}
	if (!select->groups.group_expressions.empty()) {
		GroupingSet grouping_set;
		for (idx_t i = 0; i < select->groups.group_expressions.size(); i++) {
			grouping_set.insert(i);
		}
		select->groups.grouping_sets.push_back(std::move(grouping_set));
	}

	for (auto &expr : metric_expressions) {
		select->select_list.push_back(std::move(expr));
	}

	unique_ptr<TableRef> from = BindTable(root);
	for (auto &planned : PlanJoins(model, root, required)) {
		auto join = make_uniq<JoinRef>(JoinRefType::REGULAR);
		// Ossie does not declare a join type. INNER matches hand-written SQL, but drops fact rows
		// where the foreign key is NULL; an override belongs here when one is needed.
		join->type = JoinType::INNER;
		join->left = std::move(from);
		join->right = BindTable(*planned.dataset);
		join->condition = JoinCondition(*planned.relationship);
		from = std::move(join);
	}

	select->from_table = std::move(from);
	select->aggregate_handling = AggregateHandling::STANDARD_HANDLING;

	SelectStatement statement;
	statement.node = std::move(select);
	return statement.ToString();
}

} // namespace ossie
} // namespace duckdb
