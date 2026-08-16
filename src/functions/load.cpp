#include "ossie/catalog.hpp"
#include "ossie/functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
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
                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto result = make_uniq<LoadBindData>();
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		throw InvalidInputException("ossie_load: a model path is required");
	}
	result->path = input.inputs[0].GetValue<string>();

	for (auto &entry : input.named_parameters) {
		if (StringUtil::CIEquals(entry.first, "rebind")) {
			result->rebind = ParseRebind(entry.second);
		} else if (StringUtil::CIEquals(entry.first, "validate_sources")) {
			result->validate_sources = entry.second.GetValue<bool>();
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

	output.SetCardinality(1);
	output.SetValue(0, 0, Value(model->name));
	output.SetValue(1, 0, model->spec_version.empty() ? Value(LogicalType::VARCHAR) : Value(model->spec_version));
	output.SetValue(2, 0, Value::BIGINT(static_cast<int64_t>(model->datasets.size())));
	output.SetValue(3, 0, Value::BIGINT(static_cast<int64_t>(field_count)));
	output.SetValue(4, 0, Value::BIGINT(static_cast<int64_t>(model->relationships.size())));
	output.SetValue(5, 0, Value::BIGINT(static_cast<int64_t>(model->metrics.size())));

	OssieState::Get(context).SetModel(std::move(model));
	global_state.done = true;
}

} // namespace

OssieState &OssieState::Get(ClientContext &context) {
	return *context.registered_state->GetOrCreate<OssieState>(STATE_KEY);
}

void OssieState::SetModel(shared_ptr<Model> new_model) {
	lock_guard<mutex> guard(lock);
	model = std::move(new_model);
}

shared_ptr<Model> OssieState::GetModel() {
	lock_guard<mutex> guard(lock);
	return model;
}

void RegisterLoadFunction(ExtensionLoader &loader) {
	TableFunction ossie_load("ossie_load", {LogicalType::VARCHAR}, LoadFunction, LoadBind, LoadInitGlobal);
	ossie_load.named_parameters["rebind"] = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	ossie_load.named_parameters["validate_sources"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(ossie_load);
}

} // namespace ossie
} // namespace duckdb
