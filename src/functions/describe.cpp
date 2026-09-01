#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "ossie/catalog.hpp"
#include "ossie/describe_functions.hpp"
#include "ossie/functions.hpp"
#include "ossie/state.hpp"

namespace duckdb {
namespace ossie {

namespace {

//! Holds the model for the duration of the scan so a concurrent ossie_load cannot pull it away.
struct DescribeGlobalState : public GlobalTableFunctionState {
	shared_ptr<Model> model;
	idx_t offset = 0;
	//! Only ossie_fields uses this, to walk fields nested inside datasets.
	idx_t dataset_offset = 0;
};

unique_ptr<GlobalTableFunctionState> DescribeInit(ClientContext &context, TableFunctionInitInput &input) {
	auto result = make_uniq<DescribeGlobalState>();
	result->model = OssieState::Get(context).GetModel();
	if (!result->model) {
		throw InvalidInputException("no Ossie model is loaded -- call ossie_load('model.json') first");
	}
	return std::move(result);
}

Value StringList(const vector<string> &values) {
	vector<Value> children;
	for (auto &value : values) {
		children.emplace_back(value);
	}
	return Value::LIST(LogicalType::VARCHAR, std::move(children));
}

unique_ptr<FunctionData> RelationshipsBind(ClientContext &context, TableFunctionBindInput &input,
                                           vector<LogicalType> &return_types, vector<string> &names) {
	names = {"name", "from_dataset", "to_dataset", "from_columns", "to_columns", "cardinality"};
	return_types = {LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::LIST(LogicalType::VARCHAR),
	                LogicalType::LIST(LogicalType::VARCHAR),
	                LogicalType::VARCHAR};
	return nullptr;
}

void RelationshipsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DescribeGlobalState>();
	auto &relationships = state.model->relationships;

	idx_t count = 0;
	while (state.offset < relationships.size() && count < STANDARD_VECTOR_SIZE) {
		auto &relationship = relationships[state.offset++];
		output.SetValue(0, count, Value(relationship.name));
		output.SetValue(1, count, Value(relationship.from_dataset));
		output.SetValue(2, count, Value(relationship.to_dataset));
		output.SetValue(3, count, StringList(relationship.from_columns));
		output.SetValue(4, count, StringList(relationship.to_columns));
		output.SetValue(5, count, Value(CardinalityName(relationship.cardinality)));
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> DatasetsBind(ClientContext &context, TableFunctionBindInput &input,
                                      vector<LogicalType> &return_types, vector<string> &names) {
	names = {"name", "source", "source_bound", "resolved", "primary_key", "unique_keys", "description", "synonyms"};
	return_types = {LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::VARCHAR,
	                LogicalType::BOOLEAN,
	                LogicalType::LIST(LogicalType::VARCHAR),
	                LogicalType::LIST(LogicalType::LIST(LogicalType::VARCHAR)),
	                LogicalType::VARCHAR,
	                LogicalType::LIST(LogicalType::VARCHAR)};
	return nullptr;
}

void DatasetsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DescribeGlobalState>();
	auto &datasets = state.model->datasets;

	idx_t count = 0;
	while (state.offset < datasets.size() && count < STANDARD_VECTOR_SIZE) {
		auto &dataset = datasets[state.offset++];
		vector<Value> unique_keys;
		for (auto &unique_key : dataset.unique_keys) {
			unique_keys.push_back(StringList(unique_key));
		}

		output.SetValue(0, count, Value(dataset.name));
		output.SetValue(1, count, Value(dataset.source_raw));
		output.SetValue(2, count, Value(dataset.source_bound));
		output.SetValue(3, count, Value::BOOLEAN(SourceResolves(context, dataset.source_bound)));
		output.SetValue(4, count, StringList(dataset.primary_key));
		output.SetValue(5, count, Value::LIST(LogicalType::LIST(LogicalType::VARCHAR), std::move(unique_keys)));
		output.SetValue(6, count, Value(dataset.description));
		output.SetValue(7, count, StringList(dataset.ai_context.synonyms));
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> FieldsBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<string> &names) {
	names = {"dataset", "name", "datatype", "is_time", "is_computed", "expression", "description", "synonyms"};
	return_types = {
	    LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN,
	    LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)};
	return nullptr;
}

// Fields are emitted as one flat stream across datasets, so the offset indexes the pair.
void FieldsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DescribeGlobalState>();
	auto &datasets = state.model->datasets;

	idx_t count = 0;
	while (state.dataset_offset < datasets.size() && count < STANDARD_VECTOR_SIZE) {
		auto &dataset = datasets[state.dataset_offset];
		if (state.offset >= dataset.fields.size()) {
			state.dataset_offset++;
			state.offset = 0;
			continue;
		}
		auto &field = dataset.fields[state.offset++];

		output.SetValue(0, count, Value(dataset.name));
		output.SetValue(1, count, Value(field.name));
		output.SetValue(2, count, field.datatype.empty() ? Value(LogicalType::VARCHAR) : Value(field.datatype));
		output.SetValue(3, count, Value::BOOLEAN(field.is_time));
		output.SetValue(4, count, Value::BOOLEAN(field.is_computed));
		output.SetValue(5, count, Value(field.expression.sql));
		output.SetValue(6, count, Value(field.description));
		output.SetValue(7, count, StringList(field.ai_context.synonyms));
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<FunctionData> MetricsBind(ClientContext &context, TableFunctionBindInput &input,
                                     vector<LogicalType> &return_types, vector<string> &names) {
	names = {"name", "datatype", "expression", "description", "synonyms"};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::LIST(LogicalType::VARCHAR)};
	return nullptr;
}

void MetricsFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &state = data_p.global_state->Cast<DescribeGlobalState>();
	auto &metrics = state.model->metrics;

	idx_t count = 0;
	while (state.offset < metrics.size() && count < STANDARD_VECTOR_SIZE) {
		auto &metric = metrics[state.offset++];
		output.SetValue(0, count, Value(metric.name));
		output.SetValue(1, count, metric.datatype.empty() ? Value(LogicalType::VARCHAR) : Value(metric.datatype));
		output.SetValue(2, count, Value(metric.expression.sql));
		output.SetValue(3, count, Value(metric.description));
		output.SetValue(4, count, StringList(metric.ai_context.synonyms));
		count++;
	}
	output.SetCardinality(count);
}

} // namespace

//! Registers one description table function with the metadata duckdb_functions() exposes.
void RegisterDescribe(ExtensionLoader &loader, TableFunction function, const string &description,
                      const string &example) {
	CreateTableFunctionInfo info(std::move(function));
	info.descriptions.push_back(Describe({}, description, {example}));
	loader.RegisterFunction(std::move(info));
}

void RegisterDescribeFunctions(ExtensionLoader &loader) {
	RegisterDescribe(loader,
	                 TableFunction("ossie_relationships", {}, RelationshipsFunction, RelationshipsBind, DescribeInit),
	                 "List the loaded model's relationships with the cardinality derived from the endpoints' declared "
	                 "keys. Ossie does not state cardinality; it is inferred by checking whether a relationship's join "
	                 "columns match the target's primary_key or a unique_keys entry, and it is what makes a fan-out "
	                 "join detectable.",
	                 "SELECT name, from_dataset, to_dataset, cardinality FROM ossie_relationships()");
	RegisterDescribe(loader, TableFunction("ossie_datasets", {}, DatasetsFunction, DatasetsBind, DescribeInit),
	                 "List the loaded model's datasets: the source as written in the model, the source after "
	                 "rebinding, whether that table currently resolves, declared keys, and synonyms.",
	                 "SELECT name, source_bound, resolved FROM ossie_datasets()");
	RegisterDescribe(loader, TableFunction("ossie_fields", {}, FieldsFunction, FieldsBind, DescribeInit),
	                 "List every field in the loaded model with its datatype, whether it is a time dimension, "
	                 "whether it is computed rather than a plain column, its expression, and its synonyms. "
	                 "This is the dimension vocabulary an agent picks names from.",
	                 "SELECT dataset, name, datatype, synonyms FROM ossie_fields()");
	RegisterDescribe(loader, TableFunction("ossie_metrics", {}, MetricsFunction, MetricsBind, DescribeInit),
	                 "List the loaded model's metrics with their datatype, aggregate expression, description "
	                 "and synonyms. This is the measure vocabulary an agent picks names from.",
	                 "SELECT name, description, synonyms FROM ossie_metrics()");
}

} // namespace ossie
} // namespace duckdb
