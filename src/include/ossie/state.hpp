#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "ossie/model.hpp"

namespace duckdb {
class ClientContext;

namespace ossie {

//! The loaded model, held per connection. A second ossie_load replaces the first.
class OssieState : public ClientContextState {
public:
	static constexpr const char *STATE_KEY = "ossie";

	static OssieState &Get(ClientContext &context);

	void SetModel(shared_ptr<Model> new_model, bool allow_filter_functions);

	//! Set at load time
	bool AllowFilterFunctions();
	//! nullptr until a model is loaded. Shared so a reader survives a concurrent replace.
	shared_ptr<Model> GetModel();

private:
	mutex lock;
	shared_ptr<Model> model;
	bool allow_filter_functions = false;
};

} // namespace ossie
} // namespace duckdb
