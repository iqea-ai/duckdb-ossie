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

void RequireField(const Dataset &dataset, const string &column, const string &context) {
	if (!dataset.FindField(column)) {
		throw InvalidInputException("ossie_load: %s references column \"%s\", which dataset \"%s\" does not "
		                            "declare as a field",
		                            context, column, dataset.name);
	}
}

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
		RequireField(dataset, names.back(), context);
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
		auto dataset = model.FindDataset(names[0]);
		if (!dataset) {
			throw InvalidInputException("ossie_load: %s references dataset \"%s\", which the model does not declare",
			                            context, names[0]);
		}
		RequireField(*dataset, names[1], context);
	});
}

void ValidateRelationship(const Model &model, const Relationship &relationship) {
	auto context = StringUtil::Format("relationship \"%s\"", relationship.name);
	auto from = model.FindDataset(relationship.from_dataset);
	if (!from) {
		throw InvalidInputException("ossie_load: %s has 'from' dataset \"%s\", which the model does not declare",
		                            context, relationship.from_dataset);
	}
	auto to = model.FindDataset(relationship.to_dataset);
	if (!to) {
		throw InvalidInputException("ossie_load: %s has 'to' dataset \"%s\", which the model does not declare", context,
		                            relationship.to_dataset);
	}
	// Unlike keys, these columns are emitted into the ON clause, so they must be declared.
	for (auto &column : relationship.from_columns) {
		RequireField(*from, column, context + " 'from_columns'");
	}
	for (auto &column : relationship.to_columns) {
		RequireField(*to, column, context + " 'to_columns'");
	}
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
