#include "ossie/validate.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {
namespace ossie {

namespace {

void VisitColumnRefs(const ParsedExpression &expr, const std::function<void(const ColumnRefExpression &)> &callback) {
	if (expr.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		callback(expr.Cast<ColumnRefExpression>());
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { VisitColumnRefs(child, callback); });
}

string Render(const ColumnRefExpression &colref) {
	return StringUtil::Join(colref.column_names, ".");
}

// Deliberately no RequireField() any more. The Ossie schema requires neither that a relationship's
// columns be declared fields (`fields` is not even a required property of a dataset) nor that a
// field or metric expression reference only declared fields, and real third-party models rely on
// both -- see test/fixtures/conformance/. An undeclared name is a physical column; it is qualified
// to the bound alias and resolved by DuckDB's binder, which names the column and table if it is
// genuinely wrong.
//
// This is a narrower notion of strictness than it looks. Refusing a query the model underdetermines
// (two grains, an ambiguous path, a fan-out) prevents a wrong number. Refusing a spec-legal model
// at load prevents nothing and cannot be worked around. Note that keys were already treated this
// way, because the reference model keys store_sales on a column it never exposes.

// Key columns are deliberately not checked against declared fields. Keys only ever participate in the
// cardinality comparison, never in emitted SQL, so an undeclared one is harmless.
void ValidateKeys(const Dataset &dataset) {
	for (idx_t i = 0; i < dataset.unique_keys.size(); i++) {
		if (dataset.unique_keys[i].empty()) {
			throw InvalidInputException("ossie_load: dataset \"%s\" has an empty entry in 'unique_keys'", dataset.name);
		}
	}
}

void ValidateFieldExpression(const Dataset &dataset, const Field &field) {
	auto context = StringUtil::Format("field \"%s.%s\"", dataset.name, field.name);
	VisitColumnRefs(*field.expression.tree, [&](const ColumnRefExpression &colref) {
		auto &names = colref.column_names;
		if (names.size() > 2 || (names.size() == 2 && !StringUtil::CIEquals(names[0], dataset.name))) {
			throw InvalidInputException("ossie_load: %s references \"%s\", but a field expression may only "
			                            "reference its own dataset \"%s\" -- crossing datasets would require a join",
			                            context, Render(colref), dataset.name);
		}
	});
}

void ValidateMetricExpression(const Model &model, const Metric &metric) {
	auto context = StringUtil::Format("metric \"%s\"", metric.name);
	VisitColumnRefs(*metric.expression.tree, [&](const ColumnRefExpression &colref) {
		auto &names = colref.column_names;
		if (names.size() != 2) {
			throw InvalidInputException("ossie_load: %s references \"%s\"; a metric must qualify every column "
			                            "as dataset.field so its grain is unambiguous",
			                            context, Render(colref));
		}
		if (!model.FindDataset(names[0])) {
			throw InvalidInputException("ossie_load: %s references dataset \"%s\", which the model does not declare",
			                            context, names[0]);
		}
	});
}

void ValidateRelationship(const Model &model, const Relationship &relationship) {
	auto context = StringUtil::Format("relationship \"%s\"", relationship.name);
	if (!model.FindDataset(relationship.from_dataset)) {
		throw InvalidInputException("ossie_load: %s has 'from' dataset \"%s\", which the model does not declare",
		                            context, relationship.from_dataset);
	}
	if (!model.FindDataset(relationship.to_dataset)) {
		throw InvalidInputException("ossie_load: %s has 'to' dataset \"%s\", which the model does not declare", context,
		                            relationship.to_dataset);
	}
	// The columns are emitted into the ON clause verbatim, so they need no field declaration; the
	// endpoints resolving (above) is what matters. Widths are checked in the parser.
}

} // namespace

void ValidateModel(const Model &model) {
	for (auto &dataset : model.datasets) {
		ValidateKeys(dataset);
		for (auto &field : dataset.fields) {
			ValidateFieldExpression(dataset, field);
		}
	}

	case_insensitive_set_t relationship_names;
	for (auto &relationship : model.relationships) {
		if (!relationship_names.insert(relationship.name).second) {
			throw InvalidInputException("ossie_load: relationship \"%s\" is declared more than once",
			                            relationship.name);
		}
		ValidateRelationship(model, relationship);
	}

	for (auto &metric : model.metrics) {
		ValidateMetricExpression(model, metric);
	}
}

} // namespace ossie
} // namespace duckdb
