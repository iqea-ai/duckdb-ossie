#include "ossie/catalog.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/entry_lookup_info.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/qualified_name.hpp"

namespace duckdb {
namespace ossie {

void SplitSource(const string &source, string &catalog, string &schema, string &name) {
	catalog = INVALID_CATALOG;
	schema = INVALID_SCHEMA;
	name = string();

	auto parts = QualifiedName::ParseComponents(source);
	switch (parts.size()) {
	case 3:
		catalog = parts[0];
		schema = parts[1];
		name = parts[2];
		break;
	case 2:
		schema = parts[0];
		name = parts[1];
		break;
	case 1:
		name = parts[0];
		break;
	default:
		break;
	}
}

bool SourceResolves(ClientContext &context, const string &source) {
	string catalog, schema, name;
	SplitSource(source, catalog, schema, name);
	if (name.empty()) {
		return false;
	}

	// Looking up TABLE_ENTRY also finds views, which the spec allows as a source.
	EntryLookupInfo lookup_info(CatalogType::TABLE_ENTRY, name);
	try {
		return Catalog::GetEntry(context, catalog, schema, lookup_info, OnEntryNotFound::RETURN_NULL) != nullptr;
	} catch (const std::exception &) {
		// An unattached database throws rather than returning null.
		return false;
	}
}

} // namespace ossie
} // namespace duckdb
