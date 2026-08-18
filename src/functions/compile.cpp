#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "ossie/compile.hpp"
#include "ossie/functions.hpp"
#include "ossie/state.hpp"

namespace duckdb {
namespace ossie {

namespace {

vector<string> ToStringList(const Value &value) {
	vector<string> result;
	if (value.IsNull()) {
		return result;
	}
	for (auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw InvalidInputException("ossie_compile: argument lists may not contain NULL");
		}
		result.push_back(child.GetValue<string>());
	}
	return result;
}

void CompileFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto model = OssieState::Get(context).GetModel();
	if (!model) {
		throw InvalidInputException("ossie_compile: no Ossie model is loaded -- call ossie_load('model.json') first");
	}

	for (idx_t row = 0; row < args.size(); row++) {
		auto metrics = ToStringList(args.data[0].GetValue(row));
		auto dimensions = ToStringList(args.data[1].GetValue(row));
		auto filters = ToStringList(args.data[2].GetValue(row));
		auto sql = CompileToSQL(*model, metrics, dimensions, filters);
		result.SetValue(row, Value(sql));
	}
}

} // namespace

void RegisterCompileFunction(ExtensionLoader &loader) {
	auto string_list = LogicalType::LIST(LogicalType::VARCHAR);
	ScalarFunction ossie_compile("ossie_compile", {string_list, string_list, string_list}, LogicalType::VARCHAR,
	                             CompileFunction);
	loader.RegisterFunction(ossie_compile);
}

} // namespace ossie
} // namespace duckdb
