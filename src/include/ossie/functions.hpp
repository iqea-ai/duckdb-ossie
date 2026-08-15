#pragma once

namespace duckdb {
class ExtensionLoader;

namespace ossie {

//! Registers ossie_load(path, rebind => MAP).
void RegisterLoadFunction(ExtensionLoader &loader);

} // namespace ossie
} // namespace duckdb
