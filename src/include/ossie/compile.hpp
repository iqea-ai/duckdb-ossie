#pragma once

#include "ossie/model.hpp"

namespace duckdb {
namespace ossie {

//! Compiles a semantic request into SQL, throwing when it cannot answer.
string CompileToSQL(const Model &model, const vector<string> &metrics, const vector<string> &dimensions,
                    const vector<string> &filters);

} // namespace ossie
} // namespace duckdb
