#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {
class ClientContext;

namespace ossie {

//! Splits a 1-, 2- or 3-part "catalog.schema.table" name. Missing parts are left empty.
void SplitSource(const string &source, string &catalog, string &schema, string &name);

//! True if a dataset's bound source names a table or view that exists right now.
bool SourceResolves(ClientContext &context, const string &source);

} // namespace ossie
} // namespace duckdb
