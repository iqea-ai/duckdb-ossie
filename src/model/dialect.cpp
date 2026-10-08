#include "ossie/dialect.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

// The Ossie expression language (core-spec/expression_language.md) is a SQL subset. Most of it is
// spelled the same in DuckDB, so the tree DuckDB's parser produces is already right. This file handles
// the rest, in three kinds:
//
//   * a function DuckDB spells differently or lacks (NVL, IFF, CHARINDEX, DATEADD, ...) becomes the
//     equivalent DuckDB tree;
//   * a function DuckDB has under the same name but with a different result is wrapped so the result
//     matches: CONCAT, GREATEST and LEAST skip NULL arguments in DuckDB, while NULL in gives NULL out
//     for the language's own `||` and in standard SQL;
//   * a construct whose result the language does not define, and on which engines disagree, is
//     refused at load. Guessing would return a number that is right on one engine and wrong on the
//     next, which is the one outcome this extension exists to prevent.

namespace duckdb {
namespace ossie {

namespace {

using Children = vector<unique_ptr<ParsedExpression>>;

unique_ptr<ParsedExpression> Call(const string &name, Children children) {
	return make_uniq<FunctionExpression>(name, std::move(children));
}

unique_ptr<ParsedExpression> Concatenate(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	Children children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));
	auto result = make_uniq<FunctionExpression>("||", std::move(children));
	result->is_operator = true;
	return std::move(result);
}

unique_ptr<ParsedExpression> IsNull(unique_ptr<ParsedExpression> operand) {
	Children children;
	children.push_back(std::move(operand));
	return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, std::move(children));
}

//! COALESCE(left, right): the tree DuckDB's own parser builds for IFNULL, which is not a function in its
//! catalog but an operator.
unique_ptr<ParsedExpression> Coalesce(unique_ptr<ParsedExpression> left, unique_ptr<ParsedExpression> right) {
	auto result = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
	result->children.push_back(std::move(left));
	result->children.push_back(std::move(right));
	return std::move(result);
}

unique_ptr<ParsedExpression> Null() {
	return make_uniq<ConstantExpression>(Value());
}

unique_ptr<ParsedExpression> Integer(int32_t value) {
	return make_uniq<ConstantExpression>(Value::INTEGER(value));
}

//! CASE WHEN `condition` THEN `then_expr` ELSE `else_expr` END
unique_ptr<ParsedExpression> Case(unique_ptr<ParsedExpression> condition, unique_ptr<ParsedExpression> then_expr,
                                  unique_ptr<ParsedExpression> else_expr) {
	auto result = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(condition);
	check.then_expr = std::move(then_expr);
	result->case_checks.push_back(std::move(check));
	result->else_expr = std::move(else_expr);
	return std::move(result);
}

//! NULL if any argument is NULL, otherwise `expr`. The arguments are copied, so `expr` keeps its own.
unique_ptr<ParsedExpression> NullIfAnyNull(const Children &arguments, unique_ptr<ParsedExpression> expr) {
	unique_ptr<ParsedExpression> any_null;
	for (auto &argument : arguments) {
		auto test = IsNull(argument->Copy());
		any_null = any_null ? make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(any_null),
		                                                       std::move(test))
		                    : std::move(test);
	}
	return Case(std::move(any_null), Null(), std::move(expr));
}

void RequireArgumentCount(const FunctionExpression &function, idx_t minimum, idx_t maximum, const string &spelling,
                          const string &context) {
	auto count = function.children.size();
	if (count < minimum || count > maximum) {
		throw InvalidInputException("%s calls %s with %s argument(s); OSSIE_SQL_2026 defines it as %s", context,
		                            StringUtil::Upper(function.function_name), to_string(count), spelling);
	}
}

//! The date part of DATEADD, DATEDIFF or DATE_TRUNC. The language writes it bare (`DATEADD(day, 7, d)`),
//! which DuckDB's parser reads as a column named `day`; the quoted form (`'day'`) is accepted as well.
string DatePart(const ParsedExpression &argument, const string &function_name, const string &context) {
	string part;
	if (argument.GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &column = argument.Cast<ColumnRefExpression>();
		if (column.column_names.size() == 1) {
			part = column.column_names[0];
		}
	} else if (argument.GetExpressionClass() == ExpressionClass::CONSTANT) {
		auto &constant = argument.Cast<ConstantExpression>();
		if (!constant.value.IsNull() && constant.value.type().InternalType() == PhysicalType::VARCHAR) {
			part = constant.value.ToString();
		}
	}
	part = StringUtil::Lower(part);
	static const char *const PARTS[] = {"year", "quarter", "month", "week", "day", "hour", "minute", "second"};
	for (auto known : PARTS) {
		if (part == known) {
			return part;
		}
	}
	throw InvalidInputException("%s calls %s with date part %s; OSSIE_SQL_2026 defines year, "
	                            "quarter, month, week, day, hour, minute and second",
	                            context, StringUtil::Upper(function_name), argument.ToString());
}

//! Returns the replacement for `function`, or nullptr to keep it as it is.
unique_ptr<ParsedExpression> LowerFunction(FunctionExpression &function, const string &context) {
	if (function.is_operator || !function.schema.empty() || !function.catalog.empty()) {
		return nullptr;
	}
	auto name = StringUtil::Lower(function.function_name);
	auto &args = function.children;

	if (name == "nvl") {
		RequireArgumentCount(function, 2, 2, "NVL(expr, default)", context);
		return Coalesce(std::move(args[0]), std::move(args[1]));
	}
	if (name == "iff") {
		// DuckDB's parser turns IF into this CASE itself; IFF is the same function under another name.
		RequireArgumentCount(function, 3, 3, "IFF(condition, true_result, false_result)", context);
		return Case(std::move(args[0]), std::move(args[1]), std::move(args[2]));
	}
	if (name == "nvl2") {
		RequireArgumentCount(function, 3, 3, "NVL2(expr, not_null_result, null_result)", context);
		return Case(IsNull(std::move(args[0])), std::move(args[2]), std::move(args[1]));
	}
	if (name == "zeroifnull") {
		RequireArgumentCount(function, 1, 1, "ZEROIFNULL(expr)", context);
		return Coalesce(std::move(args[0]), Integer(0));
	}
	if (name == "nullifzero") {
		RequireArgumentCount(function, 1, 1, "NULLIFZERO(expr)", context);
		args.push_back(Integer(0));
		return Call("nullif", std::move(args));
	}
	if (name == "charindex") {
		RequireArgumentCount(function, 2, 2, "CHARINDEX(substr, str)", context);
		Children swapped;
		swapped.push_back(std::move(args[1]));
		swapped.push_back(std::move(args[0]));
		return Call("strpos", std::move(swapped));
	}
	if (name == "startswith") {
		RequireArgumentCount(function, 2, 2, "STARTSWITH(str, prefix)", context);
		return Call("starts_with", std::move(args));
	}
	if (name == "endswith") {
		RequireArgumentCount(function, 2, 2, "ENDSWITH(str, suffix)", context);
		return Call("suffix", std::move(args));
	}
	if (name == "to_date" || name == "to_timestamp") {
		auto is_date = name == "to_date";
		if (args.size() != 1) {
			throw InvalidInputException(
			    "%s calls %s with a format argument. Format models differ between engines and "
			    "the format-string form is EXPERIMENTAL in OSSIE_SQL_2026; write the value as an ISO-8601 "
			    "string, which %s(string) parses the same everywhere",
			    context, StringUtil::Upper(name), StringUtil::Upper(name));
		}
		return make_uniq<CastExpression>(is_date ? LogicalType::DATE : LogicalType::TIMESTAMP, std::move(args[0]));
	}
	if (name == "dateadd") {
		RequireArgumentCount(function, 3, 3, "DATEADD(part, amount, date_expr)", context);
		auto part = DatePart(*args[0], name, context);
		Children interval;
		interval.push_back(std::move(args[1]));
		Children added;
		added.push_back(std::move(args[2]));
		added.push_back(Call("to_" + part + "s", std::move(interval)));
		return Call("date_add", std::move(added));
	}
	if (name == "datediff" || name == "date_trunc") {
		auto is_diff = name == "datediff";
		RequireArgumentCount(function, is_diff ? 3 : 2, is_diff ? 3 : 2,
		                     is_diff ? "DATEDIFF(part, start_date, end_date)" : "DATE_TRUNC(part, date_expr)", context);
		args[0] = make_uniq<ConstantExpression>(Value(DatePart(*args[0], name, context)));
		return Call(name, std::move(args));
	}
	if (name == "concat") {
		if (args.empty()) {
			RequireArgumentCount(function, 1, NumericLimits<idx_t>::Maximum(), "CONCAT(str1, str2, ...)", context);
		}
		auto result = std::move(args[0]);
		for (idx_t i = 1; i < args.size(); i++) {
			result = Concatenate(std::move(result), std::move(args[i]));
		}
		return result;
	}
	if (name == "greatest" || name == "least") {
		auto native = Call(name, Children());
		auto &native_args = native->Cast<FunctionExpression>().children;
		for (auto &argument : args) {
			native_args.push_back(argument->Copy());
		}
		return NullIfAnyNull(args, std::move(native));
	}
	if (name == "regexp_like") {
		throw InvalidInputException(
		    "%s calls REGEXP_LIKE, whose result OSSIE_SQL_2026 does not define: engines disagree on "
		    "whether the pattern must match the whole string (Snowflake) or any part of it (Databricks, "
		    "BigQuery). Use LIKE, or give the expression an ANSI_SQL variant that says which you mean",
		    context);
	}
	return nullptr;
}

//! TIMESTAMP_NTZ is the language's wall-clock timestamp; DuckDB calls it TIMESTAMP.
void LowerCast(CastExpression &cast) {
	if (cast.cast_type.id() != LogicalTypeId::UNBOUND) {
		return;
	}
	auto &type_expr = UnboundType::GetTypeExpression(cast.cast_type);
	if (!type_expr || type_expr->GetExpressionClass() != ExpressionClass::TYPE) {
		return;
	}
	if (StringUtil::CIEquals(type_expr->Cast<TypeExpression>().GetTypeName(), "timestamp_ntz")) {
		cast.cast_type = LogicalType::TIMESTAMP;
	}
}

void Lower(unique_ptr<ParsedExpression> &expr, const string &context) {
	ParsedExpressionIterator::EnumerateChildren(*expr,
	                                            [&](unique_ptr<ParsedExpression> &child) { Lower(child, context); });
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::FUNCTION: {
		auto replacement = LowerFunction(expr->Cast<FunctionExpression>(), context);
		if (replacement) {
			replacement->SetAlias(expr->GetAlias());
			expr = std::move(replacement);
		}
		break;
	}
	case ExpressionClass::CAST:
		LowerCast(expr->Cast<CastExpression>());
		break;
	default:
		break;
	}
}

} // namespace

void LowerOssieSql2026(unique_ptr<ParsedExpression> &expr, const string &context) {
	Lower(expr, context);
}

} // namespace ossie
} // namespace duckdb
