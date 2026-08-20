#pragma once

#include "ossie/model.hpp"

namespace duckdb {
class SelectStatement;

namespace ossie {

struct CompileOptions {
	//! Whether filters may call named functions. Operators are always allowed, subqueries never.
	bool allow_filter_functions = false;
};

//! Compiles a semantic request, throwing when it cannot answer.
unique_ptr<SelectStatement> Compile(const Model &model, const vector<string> &metrics, const vector<string> &dimensions,
                                    const vector<string> &filters, const CompileOptions &options);

//! The same query rendered as text. ossie_query executes the statement instead, so the SQL shown
//! here and the SQL run cannot drift apart.
string CompileToSQL(const Model &model, const vector<string> &metrics, const vector<string> &dimensions,
                    const vector<string> &filters, const CompileOptions &options);

} // namespace ossie
} // namespace duckdb
