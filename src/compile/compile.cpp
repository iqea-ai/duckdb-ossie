#include "ossie/compile.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
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

//! Named aggregates recognised in a filter, which routes the predicate to HAVING. Not exhaustive:
//! anything missed lands in WHERE and DuckDB rejects it with its own message.
bool IsAggregateName(const string &name) {
	static const case_insensitive_set_t AGGREGATES {
	    "sum",     "count",    "avg",        "min",      "max",        "stddev",  "median",
	    "mode",    "variance", "string_agg", "list",     "first",      "last",    "bool_and",
	    "bool_or", "arg_max",  "arg_min",    "quantile", "count_star", "product", "histogram"};
	return AGGREGATES.find(name) != AGGREGATES.end();
}

//! Filters come from the caller, so they are allowlisted: operators always, named calls on request,
//! subqueries never.
void CheckFilterNode(const ParsedExpression &expr, const string &request, const CompileOptions &options) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF:
	case ExpressionClass::CONSTANT:
	case ExpressionClass::COMPARISON:
	case ExpressionClass::CONJUNCTION:
	case ExpressionClass::OPERATOR:
	case ExpressionClass::BETWEEN:
	case ExpressionClass::CASE:
	case ExpressionClass::CAST:
		return;
	case ExpressionClass::FUNCTION: {
		auto &function = expr.Cast<FunctionExpression>();
		// Arithmetic, || and LIKE all arrive here with is_operator set.
		if (function.is_operator || IsAggregateName(function.function_name) || options.allow_filter_functions) {
			return;
		}
		throw InvalidInputException("ossie_compile: filter \"%s\" calls function \"%s\". Load the model with "
		                            "allow_filter_functions => true to permit function calls in filters",
		                            request, function.function_name);
	}
	case ExpressionClass::SUBQUERY:
		throw InvalidInputException("ossie_compile: filter \"%s\" contains a subquery, which could read tables the "
		                            "model does not declare",
		                            request);
	default:
		throw InvalidInputException("ossie_compile: filter \"%s\" uses an expression that is not allowed in a filter",
		                            request);
	}
}

struct Filter {
	unique_ptr<ParsedExpression> expr;
	//! Aggregates cannot appear in WHERE, so such a predicate goes to HAVING whole rather than
	//! being split -- HAVING may also reference grouped columns, so the halves stay valid together.
	bool is_aggregate = false;
};

//! Resolves the names inside a filter: `dataset.field` inlines to the field's expression, and a
//! bare metric name inlines to the metric's, which makes the predicate an aggregate.
void ResolveFilterNames(const Model &model, unique_ptr<ParsedExpression> &expr, const string &request,
                        bool &is_aggregate) {
	if (expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (colref.column_names.size() == 1) {
			auto metric = model.FindMetric(colref.column_names[0]);
			if (!metric) {
				throw InvalidInputException(
				    "ossie_compile: filter \"%s\" references \"%s\", which is neither a "
				    "metric nor a dataset.field name.%s",
				    request, colref.column_names[0],
				    StringUtil::CandidatesErrorMessage(MetricNames(model), colref.column_names[0], "Did you mean"));
			}
			auto replacement = metric->expression.tree->Copy();
			InlineFields(model, replacement);
			expr = std::move(replacement);
			is_aggregate = true;
			return;
		}
		if (colref.column_names.size() != 2) {
			throw InvalidInputException("ossie_compile: filter \"%s\" references \"%s\"; columns must be qualified "
			                            "as dataset.field",
			                            request, StringUtil::Join(colref.column_names, "."));
		}
		auto dataset = model.FindDataset(colref.column_names[0]);
		if (!dataset) {
			throw InvalidInputException("ossie_compile: filter \"%s\" references dataset \"%s\", which the model does "
			                            "not declare",
			                            request, colref.column_names[0]);
		}
		if (!dataset->FindField(colref.column_names[1])) {
			throw InvalidInputException(
			    "ossie_compile: filter \"%s\" references \"%s.%s\", which the model does not declare.%s", request,
			    colref.column_names[0], colref.column_names[1],
			    StringUtil::CandidatesErrorMessage(
			        DimensionNames(model), colref.column_names[0] + "." + colref.column_names[1], "Did you mean"));
		}
		auto replacement = dataset->FindField(colref.column_names[1])->expression.tree->Copy();
		QualifyColumns(replacement, AliasFor(*dataset));
		expr = std::move(replacement);
		return;
	}

	if (expr->GetExpressionClass() == ExpressionClass::FUNCTION &&
	    IsAggregateName(expr->Cast<FunctionExpression>().function_name)) {
		is_aggregate = true;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ResolveFilterNames(model, child, request, is_aggregate); });
}

void CheckFilterTree(const ParsedExpression &expr, const string &request, const CompileOptions &options) {
	CheckFilterNode(expr, request, options);
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { CheckFilterTree(child, request, options); });
}

Filter ResolveFilter(const Model &model, const string &request, const CompileOptions &options) {
	vector<unique_ptr<ParsedExpression>> parsed;
	try {
		parsed = Parser::ParseExpressionList(request);
	} catch (const ParserException &ex) {
		throw InvalidInputException("ossie_compile: filter \"%s\" does not parse: %s", request,
		                            ErrorData(ex).RawMessage());
	}
	if (parsed.size() != 1) {
		throw InvalidInputException("ossie_compile: filter \"%s\" must be a single predicate", request);
	}

	// Policy is checked before names are resolved, so the message names what the caller wrote.
	CheckFilterTree(*parsed[0], request, options);

	Filter result;
	result.expr = std::move(parsed[0]);
	ResolveFilterNames(model, result.expr, request, result.is_aggregate);
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
                    const vector<string> &filters, const CompileOptions &options) {
	if (metrics.empty()) {
		throw InvalidInputException("ossie_compile: at least one metric is required");
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

	// Filters may reference datasets nothing else does, which pulls in a join.
	for (auto &request : filters) {
		auto filter = ResolveFilter(model, request, options);
		case_insensitive_set_t referenced;
		CollectDatasets(*filter.expr, referenced);
		for (auto &name : referenced) {
			if (!StringUtil::CIEquals(name, root.name)) {
				required.insert(name);
			}
		}

		auto &target = filter.is_aggregate ? select->having : select->where_clause;
		if (!target) {
			target = std::move(filter.expr);
		} else {
			target = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(target),
			                                          std::move(filter.expr));
		}
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
