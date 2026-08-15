#pragma once

#include "duckdb.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {
namespace ossie {

//! `ai_context` contains vocabulary for LLM consumers. An agent can discover what it can ask for by reading synonyms.
struct AIContext {
	vector<string> synonyms;
	string instructions;
};

struct ModelExpression {
	string dialect;
	// the sql fragment as written in the spec, used for error messages
	string sql;
	unique_ptr<ParsedExpression> tree;
	vector<string> available_dialects;
};

struct Field {
	string name;
	ModelExpression expression;
	//! Ossie logical type (String/Integer/Decimal/...). May be empty. Independent of physical representation.
	string datatype;
	string label;
	string description;
	//! `dimension.is_time`
	bool is_time = false;
	AIContext ai_context;
	//! False only if the expression is exactly a reference to this field's own name
	bool is_computed;
};

struct Dataset {
	string name;
	//! `source` exactly as in the spec
	string source_raw;
	//! `source` after rebind mapping
	string source_bound;
	vector<string> primary_key;
	vector<vector<string>> unique_keys;
	string description;
	AIContext ai_context;
	vector<Field> fields;
	case_insensitive_map_t<idx_t> field_index;

	const Field *FindField(const string &field_name) const {
		auto entry = field_index.find(field_name);
		return entry == field_index.end() ? nullptr : &fields[entry->second];
	}

	//! Appends a field and keeps field_index in sync. Use this rather than touching fields directly.
	void AddField(Field field) {
		if (field_index.find(field.name) != field_index.end()) {
			throw InvalidInputException("ossie_load: dataset \"%s\" declares field \"%s\" more than once", name,
			                            field.name);
		}
		field_index[field.name] = fields.size();
		fields.push_back(std::move(field));
	}
};

struct Relationship {
	string name;
	string from_dataset;
	string to_dataset;
	vector<string> from_columns;
	vector<string> to_columns;
	AIContext ai_context;
};

struct Metric {
	string name;
	ModelExpression expression;
	//! Optional logical datatype
	string datatype;
	string description;
	AIContext ai_context;
};

struct Model {
	//! Top-level `version:` of the Ossie document, e.g. "0.2.0.dev0".
	string spec_version;
	string name;
	string description;
	AIContext ai_context;
	vector<Dataset> datasets;
	vector<Relationship> relationships;
	vector<Metric> metrics;

	case_insensitive_map_t<idx_t> dataset_index;
	case_insensitive_map_t<idx_t> metric_index;

	const Dataset *FindDataset(const string &dataset_name) const {
		auto entry = dataset_index.find(dataset_name);
		return entry == dataset_index.end() ? nullptr : &datasets[entry->second];
	}
	const Metric *FindMetric(const string &metric_name) const {
		auto entry = metric_index.find(metric_name);
		return entry == metric_index.end() ? nullptr : &metrics[entry->second];
	}
};

//! Remaps warehouse-qualified `source` prefixes onto the local catalog,
//! e.g. 'tpcds.public' -> 'tpcds.main'.
struct RebindMap {
	vector<pair<string, string>> rules;
	string Apply(const string &source) const;
};

} // namespace ossie
} // namespace duckdb
