#include "duckdb/common/exception.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "ossie/compile.hpp"
#include "ossie/describe_functions.hpp"
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
	CreateTableFunctionInfo info(std::move(ossie_query));
	auto describe = [&list](vector<string> params) {
		vector<LogicalType> types(params.size(), list);
		return Describe(std::move(params),
		                "Answer a question against the loaded semantic model: compile the request and "
		                "execute it. Returns one column per dimension followed by one per metric, "
		                "grouped by the dimensions. The compiled statement is handed to DuckDB before "
		                "binding, so join reordering, filter pushdown, and projection pruning come from "
		                "the optimizer. Refuses rather than guesses where the model underdetermines the "
		                "query -- metrics at more than one grain, joins that would fan out, or two join "
		                "paths that could give two different numbers.",
		                {"SELECT * FROM ossie_query(['total_sales'])",
		                 "SELECT * FROM ossie_query(['total_sales'], ['item.i_brand'])",
		                 "SELECT * FROM ossie_query(['total_sales'], ['item.i_brand'], "
		                 "['date_dim.d_year = 2001'])"},
		                std::move(types));
	};
	info.descriptions.push_back(describe({"metrics"}));
	info.descriptions.push_back(describe({"metrics", "dimensions"}));
	info.descriptions.push_back(describe({"metrics", "dimensions", "filters"}));
	loader.RegisterFunction(std::move(info));
}

} // namespace ossie
} // namespace duckdb
