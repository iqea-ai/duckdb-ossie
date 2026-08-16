#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {
class ClientContext;

namespace ossie {

//! True if a dataset's bound source names a table or view that exists right now.
bool SourceResolves(ClientContext &context, const string &source);

} // namespace ossie
} // namespace duckdb
