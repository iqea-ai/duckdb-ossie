#pragma once

namespace duckdb {
class ExtensionLoader;

namespace ossie {

//! Registers ossie_load(path, rebind => MAP).
void RegisterLoadFunction(ExtensionLoader &loader);

//! Registers the model description table functions.
void RegisterDescribeFunctions(ExtensionLoader &loader);

//! Registers ossie_compile(metrics, dimensions, filters).
void RegisterCompileFunction(ExtensionLoader &loader);

} // namespace ossie
} // namespace duckdb
