#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "ossie/compile.hpp"
#include "ossie/describe_functions.hpp"
#include "ossie/functions.hpp"
#include "ossie/state.hpp"

namespace duckdb {
namespace ossie {

namespace {

void CompileFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &ossie_state = OssieState::Get(context);
	auto model = ossie_state.GetModel();
	if (!model) {
		throw InvalidInputException("ossie_compile: no Ossie model is loaded -- call ossie_load('model.json') first");
	}

	for (idx_t row = 0; row < args.size(); row++) {
		auto metrics = ToStringList(args.data[0].GetValue(row), "ossie_compile");
		auto dimensions = ToStringList(args.data[1].GetValue(row), "ossie_compile");
		auto filters = ToStringList(args.data[2].GetValue(row), "ossie_compile");
		CompileOptions options;
		options.allow_filter_functions = ossie_state.AllowFilterFunctions();
		auto sql = CompileToSQL(*model, metrics, dimensions, filters, options);
		result.SetValue(row, Value(sql));
	}
}

} // namespace

vector<string> ToStringList(const Value &value, const char *function_name) {
	vector<string> result;
	if (value.IsNull()) {
		return result;
	}
	for (auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw InvalidInputException("%s: argument lists may not contain NULL", function_name);
		}
		result.push_back(child.GetValue<string>());
	}
	return result;
}

void RegisterCompileFunction(ExtensionLoader &loader) {
	auto string_list = LogicalType::LIST(LogicalType::VARCHAR);
	ScalarFunction ossie_compile("ossie_compile", {string_list, string_list, string_list}, LogicalType::VARCHAR,
	                             CompileFunction);

	CreateScalarFunctionInfo info(ossie_compile);
	info.descriptions.push_back(
	    Describe({"metrics", "dimensions", "filters"},
	             "Compile a semantic request into SQL and return it as text without executing it. Runs the "
	             "identical compiler ossie_query does, so the SQL shown here is the SQL that would run. Useful "
	             "wherever DuckDB is not the executor, and needs no tables to exist.",
	             {"SELECT ossie_compile(['total_sales'], ['item.i_brand'], [])",
	              "SELECT ossie_compile(['total_sales'], [], ['date_dim.d_year = 2001'])"}));
	loader.RegisterFunction(std::move(info));
}

} // namespace ossie
} // namespace duckdb
