#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
namespace ossie {

//! Dialects whose expressions this extension can execute, in order of preference when an expression
//! carries more than one. OSSIE_SQL_2026 comes first: the spec defines it, and names it the default.
//! constexpr rather than defined in dialect.cpp: parser.cpp builds a namespace-scope array from these, and
//! a definition in another translation unit could still be uninitialized when that array is.
constexpr const char *OSSIE_SQL_2026 = "OSSIE_SQL_2026";
constexpr const char *ANSI_SQL = "ANSI_SQL";

//! Rewrite an OSSIE_SQL_2026 expression tree in place into one DuckDB executes with the meaning the
//! Ossie expression language gives it. Functions DuckDB lacks or spells differently are replaced by
//! equivalent trees. A construct whose result the language leaves undefined, or that has no faithful
//! DuckDB equivalent, is refused with an InvalidInputException naming it, so a model fails at load
//! rather than returning a number nobody specified. `context` begins every error: the caller
//! prefixes it, "ossie_load: field ..." at load or "ossie: filter ..." for a request.
void LowerOssieSql2026(unique_ptr<ParsedExpression> &expr, const string &context);

} // namespace ossie
} // namespace duckdb
