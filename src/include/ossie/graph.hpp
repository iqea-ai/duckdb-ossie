#pragma once

#include "ossie/model.hpp"

namespace duckdb {
namespace ossie {

//! Fills in each relationship's cardinality from the endpoints' declared keys.
void ComputeCardinality(Model &model);

} // namespace ossie
} // namespace duckdb
