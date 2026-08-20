#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "ossie/compile.hpp"
#include "ossie/functions.hpp"
#include "ossie/state.hpp"

namespace duckdb {
namespace ossie {

namespace {

// bind_replace runs before binding, so the generated SELECT is planned as though it had been
// written by hand: join order, filter pushdown and projection pruning all come from the optimizer.
unique_ptr<TableRef> QueryBindReplace(ClientContext &context, TableFunctionBindInput &input) {
	auto &ossie_state = OssieState::Get(context);
	auto model = ossie_state.GetModel();
	if (!model) {
		throw InvalidInputException("ossie_query: no Ossie model is loaded -- call ossie_load('model.json') first");
	}

	CompileOptions options;
	options.allow_filter_functions = ossie_state.AllowFilterFunctions();

	auto argument = [&](idx_t index) {
		return index < input.inputs.size() ? ToStringList(input.inputs[index], "ossie_query") : vector<string>();
	};
	auto statement = Compile(*model, argument(0), argument(1), argument(2), options);
	return make_uniq<SubqueryRef>(std::move(statement));
}

} // namespace

void RegisterQueryFunction(ExtensionLoader &loader) {
	auto list = LogicalType::LIST(LogicalType::ANY);
	TableFunctionSet ossie_query("ossie_query");
	for (idx_t arity = 1; arity <= 3; arity++) {
		TableFunction variant("ossie_query", vector<LogicalType>(arity, list), nullptr);
		variant.bind_replace = QueryBindReplace;
		ossie_query.AddFunction(std::move(variant));
	}
	loader.RegisterFunction(std::move(ossie_query));
}

} // namespace ossie
} // namespace duckdb
