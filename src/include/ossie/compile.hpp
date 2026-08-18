#pragma once

#include "ossie/model.hpp"

namespace duckdb {
namespace ossie {

struct CompileOptions {
	//! Whether filters may call named functions. Operators are always allowed, subqueries never.
	bool allow_filter_functions = false;
};

//! Compiles a semantic request into SQL, throwing when it cannot answer.
string CompileToSQL(const Model &model, const vector<string> &metrics, const vector<string> &dimensions,
                    const vector<string> &filters, const CompileOptions &options);

} // namespace ossie
} // namespace duckdb
