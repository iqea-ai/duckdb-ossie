#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"

namespace duckdb {
namespace ossie {

//! The parts of a column reference as plain strings. DuckDB holds them as Identifiers behind an accessor; the model
//! is keyed by string throughout, so compare and look up through this.
inline vector<string> ColumnNames(const ColumnRefExpression &colref) {
	vector<string> names;
	for (auto &name : colref.ColumnNames()) {
		names.push_back(name.GetIdentifierName());
	}
	return names;
}

} // namespace ossie
} // namespace duckdb
