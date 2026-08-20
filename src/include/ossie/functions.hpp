#pragma once

#include "duckdb.hpp"

namespace duckdb {
class ExtensionLoader;

namespace ossie {

//! Reads a VARCHAR[] argument, rejecting NULL entries.
vector<string> ToStringList(const Value &value, const char *function_name);

//! Registers ossie_load(path, rebind => MAP).
void RegisterLoadFunction(ExtensionLoader &loader);

//! Registers the model description table functions.
void RegisterDescribeFunctions(ExtensionLoader &loader);

//! Registers ossie_compile(metrics, dimensions, filters).
void RegisterCompileFunction(ExtensionLoader &loader);

//! Registers ossie_query(metrics, dimensions, filters).
void RegisterQueryFunction(ExtensionLoader &loader);

} // namespace ossie
} // namespace duckdb
