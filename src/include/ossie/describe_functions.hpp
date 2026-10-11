#pragma once

#include "duckdb.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/parser/parsed_data/create_function_info.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"

namespace duckdb {
namespace ossie {

//! Builds the metadata DuckDB exposes through duckdb_functions(). This is not cosmetic: the
//! community-extensions registry generates each extension's documentation page by snapshotting
//! duckdb_functions() before and after loading the extension and diffing the two, then rendering
//! the description, parameter names, and examples it finds. Leaving them unset publishes a bare
//! list of names with empty columns.
//! `parameter_types` matters when a function has several overloads: duckdb_functions() picks which
//! description belongs to which overload by scoring the declared types against the actual ones, so a
//! description with none attached matches nothing and the row renders empty.
inline FunctionDescription Describe(vector<string> parameter_names, string description, vector<string> examples,
                                    vector<LogicalType> parameter_types = {}) {
	FunctionDescription result;
	result.parameter_types = std::move(parameter_types);
	result.parameter_names = std::move(parameter_names);
	result.description = std::move(description);
	result.examples = std::move(examples);
	result.categories = {"ossie", "semantic layer"};
	return result;
}

//! Names a function's positional parameters. DuckDB 2.0's duckdb_functions() takes parameter names from the
//! signature alone and no longer reads FunctionDescription.parameter_names, so a function built from a bare
//! type list publishes col0, col1, ... The parameters' kinds are untouched: they stay positional-only.
inline void NameParameters(FunctionSignature &signature, const vector<string> &names) {
	for (idx_t i = 0; i < names.size() && i < signature.GetParameterCount(); i++) {
		signature.GetParameter(i).SetName(Identifier(names[i]));
	}
}

} // namespace ossie
} // namespace duckdb
