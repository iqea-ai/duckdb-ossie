#include "ossie/catalog.hpp"
#include "ossie/functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "ossie/describe_functions.hpp"
#include "ossie/parser.hpp"
#include "ossie/state.hpp"

namespace duckdb {
namespace ossie {

namespace {

struct LoadBindData : public TableFunctionData {
	string path;
	RebindMap rebind;
	//! Off by default to allow ossie_compile to work with engines where the tables are not local.
	bool validate_sources = false;
	//! Set at load so a caller supplying filters cannot widen its own policy.
	bool allow_filter_functions = false;
};

struct LoadGlobalState : public GlobalTableFunctionState {
	bool done = false;
};

RebindMap ParseRebind(const Value &value) {
	RebindMap result;
	if (value.IsNull()) {
		return result;
	}
	for (auto &entry : MapValue::GetChildren(value)) {
		auto &pair = StructValue::GetChildren(entry);
		if (pair[0].IsNull() || pair[1].IsNull()) {
			throw InvalidInputException("ossie_load: 'rebind' may not contain NULL keys or values");
		}
		result.rules.emplace_back(pair[0].GetValue<string>(), pair[1].GetValue<string>());
	}
	return result;
}

unique_ptr<FunctionData> LoadBind(ClientContext &context, TableFunctionBindInput &input,
                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<LoadBindData>();
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw InvalidInputException("ossie_load: a model path is required");
	}
	result->path = input.inputs[0].GetValue<string>();

	for (auto &entry : input.named_parameters) {
		auto &name = entry.first.GetIdentifierName();
		if (StringUtil::CIEquals(name, "rebind")) {
			result->rebind = ParseRebind(entry.second);
		} else if (StringUtil::CIEquals(name, "validate_sources")) {
			result->validate_sources = entry.second.GetValue<bool>();
		} else if (StringUtil::CIEquals(name, "allow_filter_functions")) {
			result->allow_filter_functions = entry.second.GetValue<bool>();
		}
	}

	names = {"model", "spec_version", "datasets", "fields", "relationships", "metrics"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BIGINT,
	                LogicalType::BIGINT,  LogicalType::BIGINT,  LogicalType::BIGINT};
	return std::move(result);
}

unique_ptr<GlobalTableFunctionState> LoadInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<LoadGlobalState>();
}

// Parsing happens here rather than in bind so that EXPLAIN does not mutate session state.
void LoadFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &global_state = data_p.global_state->Cast<LoadGlobalState>();
	if (global_state.done) {
		return;
	}
	auto &bind_data = data_p.bind_data->Cast<LoadBindData>();

	auto model = make_shared_ptr<Model>(LoadModel(context, bind_data.path, bind_data.rebind));

	if (bind_data.validate_sources) {
		// Report every unresolved source at once, so fixing a rebind takes one round trip.
		vector<string> unresolved;
		for (auto &dataset : model->datasets) {
			if (!SourceResolves(context, dataset.source_bound)) {
				unresolved.push_back(StringUtil::Format("\"%s\" (%s)", dataset.name, dataset.source_bound));
			}
		}
		if (!unresolved.empty()) {
			throw InvalidInputException("ossie_load: %s of %s dataset sources do not exist in the catalog: %s",
			                            to_string(unresolved.size()), to_string(model->datasets.size()),
			                            StringUtil::Join(unresolved, ", "));
		}
	}

	idx_t field_count = 0;
	for (auto &dataset : model->datasets) {
		field_count += dataset.fields.size();
	}

	output.data[0].Append(Value(model->name));
	output.data[1].Append(model->spec_version.empty() ? Value(LogicalType::VARCHAR) : Value(model->spec_version));
	output.data[2].Append(Value::BIGINT(static_cast<int64_t>(model->datasets.size())));
	output.data[3].Append(Value::BIGINT(static_cast<int64_t>(field_count)));
	output.data[4].Append(Value::BIGINT(static_cast<int64_t>(model->relationships.size())));
	output.data[5].Append(Value::BIGINT(static_cast<int64_t>(model->metrics.size())));
	output.CheckCardinality(1);

	OssieState::Get(context).SetModel(std::move(model), bind_data.allow_filter_functions);
	global_state.done = true;
}

} // namespace

OssieState &OssieState::Get(ClientContext &context) {
	return *ObjectCache::GetObjectCache(context).GetOrCreate<OssieState>(STATE_KEY);
}

void OssieState::SetModel(shared_ptr<Model> new_model, bool allow_filter_functions_p) {
	lock_guard<mutex> guard(lock);
	model = std::move(new_model);
	allow_filter_functions = allow_filter_functions_p;
}

bool OssieState::AllowFilterFunctions() {
	lock_guard<mutex> guard(lock);
	return allow_filter_functions;
}

shared_ptr<Model> OssieState::GetModel() {
	lock_guard<mutex> guard(lock);
	return model;
}

void RegisterLoadFunction(ExtensionLoader &loader) {
	TableFunction ossie_load("ossie_load", {LogicalType::VARCHAR}, LoadFunction, LoadBind, LoadInitGlobal);
	// Keyword-only parameters without a default are required, so each carries the value it has when omitted.
	auto rebind_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	ossie_load.GetSignature()
	    .AddKeywordOnly("rebind", rebind_type, Value(rebind_type))
	    .AddKeywordOnly("validate_sources", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddKeywordOnly("allow_filter_functions", LogicalType::BOOLEAN, Value::BOOLEAN(false));

	CreateTableFunctionInfo info(ossie_load);
	info.descriptions.push_back(Describe(
	    {"path"},
	    "Parse and validate an Apache Ossie semantic model, in YAML or JSON, and hold it for the "
	    "database. Named parameters: rebind (MAP) remaps warehouse-qualified source prefixes onto the "
	    "local catalog; validate_sources (BOOLEAN, default false) requires every dataset's table to "
	    "exist and reports all failures at once; allow_filter_functions (BOOLEAN, default false) lets "
	    "request-supplied filters call named functions, and is set here so a caller supplying filters "
	    "cannot widen its own policy.",
	    {"CALL ossie_load('model.yaml')", "CALL ossie_load('model.json', rebind => MAP{'tpcds.public': 'memory.main'})",
	     "CALL ossie_load('model.yaml', validate_sources => true)"}));
	loader.RegisterFunction(std::move(info));
}

} // namespace ossie
} // namespace duckdb
