#include "ossie/graph.hpp"

namespace duckdb {
namespace ossie {

namespace {

//! Check for set equality: [a, b] and [b,a ] are treated as equivalent.
bool SameKey(const vector<string> &columns, const vector<string> &key) {
	if (columns.size() != key.size() || key.empty()) {
		return false;
	}
	case_insensitive_set_t remaining(key.begin(), key.end());
	for (auto &column : columns) {
		if (remaining.erase(column) == 0) {
			return false;
		}
	}
	return remaining.empty();
}

//! Unique if the columns match primary_key or any unique_keys entry - both must be checked.
bool IsUniqueKey(const Dataset &dataset, const vector<string> &columns) {
	if (SameKey(columns, dataset.primary_key)) {
		return true;
	}
	for (auto &unique_key : dataset.unique_keys) {
		if (SameKey(columns, unique_key)) {
			return true;
		}
	}
	return false;
}

} // namespace

const char *CardinalityName(Cardinality cardinality) {
	switch (cardinality) {
	case Cardinality::MANY_TO_ONE:
		return "many_to_one";
	case Cardinality::ONE_TO_MANY:
		return "one_to_many";
	case Cardinality::ONE_TO_ONE:
		return "one_to_one";
	default:
		return "many_to_many";
	}
}

void ComputeCardinality(Model &model) {
	for (auto &relationship : model.relationships) {
		// Validation has already guaranteed both endpoints resolve.
		auto from = model.FindDataset(relationship.from_dataset);
		auto to = model.FindDataset(relationship.to_dataset);
		bool from_unique = IsUniqueKey(*from, relationship.from_columns);
		bool to_unique = IsUniqueKey(*to, relationship.to_columns);

		if (from_unique && to_unique) {
			relationship.cardinality = Cardinality::ONE_TO_ONE;
		} else if (to_unique) {
			relationship.cardinality = Cardinality::MANY_TO_ONE;
		} else if (from_unique) {
			relationship.cardinality = Cardinality::ONE_TO_MANY;
		} else {
			relationship.cardinality = Cardinality::MANY_TO_MANY;
		}
	}
}

} // namespace ossie
} // namespace duckdb
