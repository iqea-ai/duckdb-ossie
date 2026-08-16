#pragma once

#include "ossie/model.hpp"

namespace duckdb {
namespace ossie {

//! Structural checks over a parsed model. Does not touch the catalog.
void ValidateModel(const Model &model);

} // namespace ossie
} // namespace duckdb
