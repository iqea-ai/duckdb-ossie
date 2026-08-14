#define DUCKDB_EXTENSION_MAIN

#include "ossie_extension.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	// ossie_load, the four introspection table functions, ossie_query and ossie_compile
	// register here as the phases come online.
	(void)loader;
}

void OssieExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string OssieExtension::Name() {
	return "ossie";
}

std::string OssieExtension::Version() const {
#ifdef EXT_VERSION_OSSIE
	return EXT_VERSION_OSSIE;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(ossie, loader) {
	duckdb::LoadInternal(loader);
}
}
