#pragma once

#include "duckdb/common/mutex.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "ossie/model.hpp"

namespace duckdb {
class ClientContext;

namespace ossie {

//! The loaded model, held per database rather than per connection: an MCP server answers on its own
//! connection, and a model loaded by the init script has to be visible there.
class OssieState : public ObjectCacheEntry {
public:
	static constexpr const char *STATE_KEY = "ossie";

	static OssieState &Get(ClientContext &context);

	void SetModel(shared_ptr<Model> new_model, bool allow_filter_functions);

	//! Set at load so a caller supplying filters cannot widen its own policy.
	bool AllowFilterFunctions();
	//! nullptr until a model is loaded. Shared so a reader survives a concurrent replace.
	shared_ptr<Model> GetModel();

	static string ObjectType() {
		return "ossie_model";
	}
	string GetObjectType() override {
		return ObjectType();
	}
	//! Invalid means non-evictable. An evicted model would look exactly like no model was loaded.
	optional_idx GetEstimatedCacheMemory() const override {
		return optional_idx();
	}

private:
	mutex lock;
	shared_ptr<Model> model;
	bool allow_filter_functions = false;
};

} // namespace ossie
} // namespace duckdb
