#include "ossie/parser.hpp"

#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/parser.hpp"

#include "yyjson.hpp"

using namespace duckdb_yyjson; // NOLINT

namespace duckdb {
namespace ossie {

namespace {

struct JSONDocument {
	explicit JSONDocument(yyjson_doc *doc_p) : doc(doc_p) {
	}
	~JSONDocument() {
		if (doc) {
			yyjson_doc_free(doc);
		}
	}
	JSONDocument(const JSONDocument &) = delete;
	JSONDocument &operator=(const JSONDocument &) = delete;

	//! yyjson_doc is a raw C handle owned by JSONDocument during its lifetime
	yyjson_doc *doc;
};

yyjson_val *Member(yyjson_val *obj, const char *key) {
	if (!obj || !yyjson_is_obj(obj)) {
		return nullptr;
	}
	return yyjson_obj_get(obj, key);
}

string RequiredString(yyjson_val *obj, const char *key, const string &context) {
	auto val = Member(obj, key);
	if (!val || !yyjson_is_str(val)) {
		throw InvalidInputException("ossie_load: %s is missing required string field '%s'", context, key);
	}
	return string(yyjson_get_str(val));
}

string OptionalString(yyjson_val *obj, const char *key) {
	auto val = Member(obj, key);
	if (!val || !yyjson_is_str(val)) {
		return string();
	}
	return string(yyjson_get_str(val));
}

vector<string> StringArray(yyjson_val *obj, const char *key, const string &context) {
	vector<string> result;
	auto arr = Member(obj, key);
	if (!arr) {
		return result;
	}
	if (!yyjson_is_arr(arr)) {
		throw InvalidInputException("ossie_load: %s field '%s' must be an array of strings", context, key);
	}
	size_t idx, max;
	yyjson_val *item;
	yyjson_arr_foreach(arr, idx, max, item) {
		if (!yyjson_is_str(item)) {
			throw InvalidInputException("ossie_load: %s field '%s' must contain only strings", context, key);
		}
		result.emplace_back(yyjson_get_str(item));
	}
	return result;
}

AIContext ParseAIContext(yyjson_val *parent, const string &context) {
	AIContext result;
	auto ai_context = Member(parent, "ai_context");
	if (!ai_context) {
		return result;
	}
	result.synonyms = StringArray(ai_context, "synonyms", context);
	result.instructions = OptionalString(ai_context, "instructions");
	return result;
}

//! Ossie's Dialect enum is {ANSI_SQL, SNOWFLAKE, MDX, TABLEAU, DATABRICKS, MAQL, BIGQUERY}
//! There is no DuckDB member (for now), so we can only execute ANSI_SQL
const char *const EXECUTABLE_DIALECT = "ANSI_SQL";

ModelExpression ParseModelExpression(yyjson_val *parent, const string &context) {
	auto expression_obj = Member(parent, "expression");
	if (!expression_obj) {
		throw InvalidInputException("ossie_load: %s is missing required field 'expression'", context);
	}
	auto dialects = Member(expression_obj, "dialects");
	if (!dialects || !yyjson_is_arr(dialects)) {
		throw InvalidInputException("ossie_load: %s has an 'expression' with no 'dialects' array", context);
	}

	ModelExpression result;
	vector<pair<string, string>> variants;
	size_t idx, max;
	yyjson_val *item;
	yyjson_arr_foreach(dialects, idx, max, item) {
		auto dialect = RequiredString(item, "dialect", context);
		auto sql = RequiredString(item, "expression", context);
		result.available_dialects.push_back(dialect);
		variants.emplace_back(dialect, sql);
	}

	for (auto &variant : variants) {
		if (StringUtil::CIEquals(variant.first, EXECUTABLE_DIALECT)) {
			result.dialect = variant.first;
			result.sql = variant.second;
			break;
		}
	}

	if (result.dialect.empty()) {
		throw InvalidInputException("ossie_load: %s has no expression in a dialect this extension can "
		                            "execute (found: %s; expected ANSI_SQL).",
		                            context, StringUtil::Join(result.available_dialects, ", "));
	}

	try {
		auto parsed = Parser::ParseExpressionList(result.sql);
		if (parsed.size() != 1) {
			throw InvalidInputException("ossie_load: %s expression '%s' parsed as %s expressions; "
			                            "expected exactly one",
			                            context, result.sql, to_string(parsed.size()));
		}
		result.tree = std::move(parsed[0]);
	} catch (const ParserException &ex) {
		throw InvalidInputException("ossie_load: %s has an unparseable expression '%s': %s", context, result.sql,
		                            ErrorData(ex).RawMessage());
	}
	return result;
}

//! True only when the expression is a bare reference to the field's own name
bool IsPlainReferenceTo(const ParsedExpression &expr, const string &field_name) {
	if (expr.GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		return false;
	}
	auto &colref = expr.Cast<ColumnRefExpression>();
	if (colref.column_names.empty()) {
		return false;
	}
	return StringUtil::CIEquals(colref.column_names.back(), field_name);
}

Field ParseField(yyjson_val *field_obj, const string &dataset_name) {
	Field result;
	result.name = RequiredString(field_obj, "name", StringUtil::Format("dataset \"%s\" field", dataset_name));
	auto context = StringUtil::Format("field \"%s.%s\"", dataset_name, result.name);

	result.expression = ParseModelExpression(field_obj, context);
	result.datatype = OptionalString(field_obj, "datatype");
	result.label = OptionalString(field_obj, "label");
	result.description = OptionalString(field_obj, "description");
	result.ai_context = ParseAIContext(field_obj, context);

	auto dimension = Member(field_obj, "dimension");
	if (dimension) {
		auto is_time = Member(dimension, "is_time");
		if (is_time && yyjson_is_bool(is_time)) {
			result.is_time = yyjson_get_bool(is_time);
		}
	}

	result.is_computed = !IsPlainReferenceTo(*result.expression.tree, result.name);
	return result;
}

//! `unique_keys` is a list of key lists, each entry is a (possibly composite) key.
vector<vector<string>> ParseUniqueKeys(yyjson_val *dataset_obj, const string &context) {
	vector<vector<string>> result;
	auto arr = Member(dataset_obj, "unique_keys");
	if (!arr) {
		return result;
	}
	if (!yyjson_is_arr(arr)) {
		throw InvalidInputException("ossie_load: %s field 'unique_keys' must be a list of column lists", context);
	}
	size_t idx, max;
	yyjson_val *item;
	yyjson_arr_foreach(arr, idx, max, item) {
		if (!yyjson_is_arr(item)) {
			throw InvalidInputException("ossie_load: %s field 'unique_keys' must be a list of column "
			                            "lists (each entry is itself an array)",
			                            context);
		}
		vector<string> key;
		size_t col_idx, col_max;
		yyjson_val *col;
		yyjson_arr_foreach(item, col_idx, col_max, col) {
			if (!yyjson_is_str(col)) {
				throw InvalidInputException("ossie_load: %s has a non-string column in 'unique_keys'", context);
			}
			key.emplace_back(yyjson_get_str(col));
		}
		result.push_back(std::move(key));
	}
	return result;
}

//! Only accept `source` that follows "database.schema.table" format, not a query
void ValidateSourceShape(const string &source, const string &context) {
	if (source.find_first_of(" \t\n(") != string::npos) {
		throw InvalidInputException("ossie_load: %s has a query source ('%s'). Only "
		                            "database.schema.table references are supported: a query source "
		                            "can neither be rebound nor bound as a table.",
		                            context, source);
	}
}

Dataset ParseDataset(yyjson_val *dataset_obj, const RebindMap &rebind) {
	Dataset result;
	result.name = RequiredString(dataset_obj, "name", "dataset");
	auto context = StringUtil::Format("dataset \"%s\"", result.name);

	result.source_raw = RequiredString(dataset_obj, "source", context);
	ValidateSourceShape(result.source_raw, context);
	result.source_bound = rebind.Apply(result.source_raw);

	result.primary_key = StringArray(dataset_obj, "primary_key", context);
	result.unique_keys = ParseUniqueKeys(dataset_obj, context);
	result.description = OptionalString(dataset_obj, "description");
	result.ai_context = ParseAIContext(dataset_obj, context);

	auto fields = Member(dataset_obj, "fields");
	if (fields) {
		if (!yyjson_is_arr(fields)) {
			throw InvalidInputException("ossie_load: %s field 'fields' must be an array", context);
		}
		size_t idx, max;
		yyjson_val *item;
		yyjson_arr_foreach(fields, idx, max, item) {
			auto field = ParseField(item, result.name);
			if (result.field_index.find(field.name) != result.field_index.end()) {
				throw InvalidInputException("ossie_load: %s declares field \"%s\" more than once", context, field.name);
			}
			result.field_index[field.name] = result.fields.size();
			result.fields.push_back(std::move(field));
		}
	}
	return result;
}

Relationship ParseRelationship(yyjson_val *obj) {
	Relationship result;
	result.name = RequiredString(obj, "name", "relationship");
	auto context = StringUtil::Format("relationship \"%s\"", result.name);

	result.from_dataset = RequiredString(obj, "from", context);
	result.to_dataset = RequiredString(obj, "to", context);
	result.from_columns = StringArray(obj, "from_columns", context);
	result.to_columns = StringArray(obj, "to_columns", context);
	result.ai_context = ParseAIContext(obj, context);

	if (result.from_columns.empty() || result.to_columns.empty()) {
		throw InvalidInputException("ossie_load: %s must declare both 'from_columns' and 'to_columns'", context);
	}
	// Refuse an unequal join key since it is not expressible as an equi-join and would produce a wrong row count.
	if (result.from_columns.size() != result.to_columns.size()) {
		throw InvalidInputException("ossie_load: %s joins %s columns to %s columns; the key widths must match", context,
		                            to_string(result.from_columns.size()), to_string(result.to_columns.size()));
	}
	return result;
}

} // namespace

string RebindMap::Apply(const string &source) const {
	const string *replacement = nullptr;
	idx_t matched_length = 0;
	for (auto &rule : rules) {
		auto &prefix = rule.first;
		// Must leave something after the prefix, and must break on a dot so that 'tpcds.pub'
		// cannot match 'tpcds.public.store_sales'.
		if (source.size() <= prefix.size() || source[prefix.size()] != '.') {
			continue;
		}
		if (!StringUtil::CIEquals(source.substr(0, prefix.size()), prefix)) {
			continue;
		}
		if (prefix.size() > matched_length) {
			matched_length = prefix.size();
			replacement = &rule.second;
		}
	}
	if (!replacement) {
		return source;
	}
	return *replacement + source.substr(matched_length);
}

Model ParseModel(const string &json_text, const RebindMap &rebind) {
	JSONDocument document(yyjson_read(json_text.c_str(), json_text.size(), 0));
	if (!document.doc) {
		throw InvalidInputException("ossie_load: file is not valid JSON");
	}
	auto root = yyjson_doc_get_root(document.doc);
	if (!yyjson_is_obj(root)) {
		throw InvalidInputException("ossie_load: the top level of the document must be a JSON object");
	}

	Model result;
	result.spec_version = OptionalString(root, "version");

	auto models = Member(root, "semantic_model");
	if (!models || !yyjson_is_arr(models)) {
		throw InvalidInputException("ossie_load: document has no 'semantic_model' array");
	}
	auto model_count = yyjson_arr_size(models);
	if (model_count == 0) {
		throw InvalidInputException("ossie_load: 'semantic_model' is empty");
	}
	if (model_count > 1) {
		// The format allows N models; ossie_query addresses metrics by bare name and can only mean
		// one. Merging them would let two datasets of the same name bind to different physical
		// tables and return an ambiguous result.
		vector<string> names;
		size_t idx, max;
		yyjson_val *item;
		yyjson_arr_foreach(models, idx, max, item) {
			names.push_back(OptionalString(item, "name"));
		}
		throw InvalidInputException("ossie_load: file contains %s semantic models (%s), but metrics are "
		                            "addressed by bare name with no way to say which model is meant. "
		                            "Split the file, or load one model per call.",
		                            to_string(model_count), StringUtil::Join(names, ", "));
	}

	auto model_obj = yyjson_arr_get(models, 0);
	result.name = RequiredString(model_obj, "name", "semantic_model");
	auto context = StringUtil::Format("semantic model \"%s\"", result.name);
	result.description = OptionalString(model_obj, "description");
	result.ai_context = ParseAIContext(model_obj, context);

	auto datasets = Member(model_obj, "datasets");
	if (!datasets || !yyjson_is_arr(datasets)) {
		throw InvalidInputException("ossie_load: %s has no 'datasets' array", context);
	}
	size_t idx, max;
	yyjson_val *item;
	yyjson_arr_foreach(datasets, idx, max, item) {
		auto dataset = ParseDataset(item, rebind);
		if (result.dataset_index.find(dataset.name) != result.dataset_index.end()) {
			throw InvalidInputException("ossie_load: %s declares dataset \"%s\" more than once", context, dataset.name);
		}
		result.dataset_index[dataset.name] = result.datasets.size();
		result.datasets.push_back(std::move(dataset));
	}

	auto relationships = Member(model_obj, "relationships");
	if (relationships) {
		if (!yyjson_is_arr(relationships)) {
			throw InvalidInputException("ossie_load: %s field 'relationships' must be an array", context);
		}
		yyjson_arr_foreach(relationships, idx, max, item) {
			result.relationships.push_back(ParseRelationship(item));
		}
	}

	auto metrics = Member(model_obj, "metrics");
	if (metrics) {
		if (!yyjson_is_arr(metrics)) {
			throw InvalidInputException("ossie_load: %s field 'metrics' must be an array", context);
		}
		yyjson_arr_foreach(metrics, idx, max, item) {
			Metric metric;
			metric.name = RequiredString(item, "name", "metric");
			auto metric_context = StringUtil::Format("metric \"%s\"", metric.name);
			metric.expression = ParseModelExpression(item, metric_context);
			metric.datatype = OptionalString(item, "datatype");
			metric.description = OptionalString(item, "description");
			metric.ai_context = ParseAIContext(item, metric_context);

			if (result.metric_index.find(metric.name) != result.metric_index.end()) {
				throw InvalidInputException("ossie_load: %s declares metric \"%s\" more than once", context,
				                            metric.name);
			}
			result.metric_index[metric.name] = result.metrics.size();
			result.metrics.push_back(std::move(metric));
		}
	}

	return result;
}

Model LoadModel(ClientContext &context, const string &path, const RebindMap &rebind) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto handle = fs.OpenFile(path, FileFlags::FILE_FLAGS_READ);
	auto file_size = handle->GetFileSize();
	string contents(file_size, '\0');
	handle->Read(reinterpret_cast<void *>(&contents[0]), file_size);
	return ParseModel(contents, rebind);
}

} // namespace ossie
} // namespace duckdb
