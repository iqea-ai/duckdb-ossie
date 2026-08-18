#include "ossie/compile.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
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

unique_ptr<TableRef> BindTable(const Dataset &dataset) {
	auto table_ref = make_uniq<BaseTableRef>();
	SplitSource(dataset.source_bound, table_ref->catalog_name, table_ref->schema_name, table_ref->table_name);
	table_ref->alias = AliasFor(dataset);
	return std::move(table_ref);
}

} // namespace

string CompileToSQL(const Model &model, const vector<string> &metrics, const vector<string> &dimensions,
                    const vector<string> &filters) {
	if (metrics.empty()) {
		throw InvalidInputException("ossie_compile: at least one metric is required");
	}
	if (!dimensions.empty() || !filters.empty()) {
		throw InvalidInputException("ossie_compile: dimensions and filters are not supported yet");
	}

	auto select = make_uniq<SelectNode>();
	case_insensitive_set_t required;

	for (auto &metric_name : metrics) {
		auto metric = model.FindMetric(metric_name);
		if (!metric) {
			throw InvalidInputException(
			    "ossie_compile: model \"%s\" has no metric named \"%s\".%s", model.name, metric_name,
			    StringUtil::CandidatesErrorMessage(MetricNames(model), metric_name, "Did you mean"));
		}
		CollectDatasets(*metric->expression.tree, required);

		auto expr = metric->expression.tree->Copy();
		InlineFields(model, expr);
		expr->SetAlias(metric->name);
		select->select_list.push_back(std::move(expr));
	}

	if (required.size() != 1) {
		throw InvalidInputException(
		    "ossie_compile: this request spans %s datasets; only single-dataset requests are supported so far",
		    to_string(required.size()));
	}

	select->from_table = BindTable(*model.FindDataset(*required.begin()));
	select->aggregate_handling = AggregateHandling::STANDARD_HANDLING;

	SelectStatement statement;
	statement.node = std::move(select);
	return statement.ToString();
}

} // namespace ossie
} // namespace duckdb
