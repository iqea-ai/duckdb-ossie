#include "ossie/yaml.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"

#include <fkYAML/node.hpp>

namespace duckdb {
namespace ossie {

namespace {

void EscapeJsonString(const std::string &value, string &out) {
	out += '"';
	for (auto c : value) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		default:
			// Control characters must be escaped; everything else (including UTF-8 continuation
			// bytes) is passed through untouched.
			if (static_cast<unsigned char>(c) < 0x20) {
				out += StringUtil::Format("\\u%04x", static_cast<int>(static_cast<unsigned char>(c)));
			} else {
				out += c;
			}
			break;
		}
	}
	out += '"';
}

void EmitJson(const fkyaml::node &node, string &out) {
	if (node.is_mapping()) {
		out += '{';
		bool first = true;
		for (auto &pair : node.as_map()) {
			if (!first) {
				out += ',';
			}
			first = false;
			// Ossie keys are always strings. A non-string key (YAML permits them) is rendered via
			// its scalar text rather than rejected, since validation will reject the unknown key.
			if (pair.first.is_string()) {
				EscapeJsonString(pair.first.as_str(), out);
			} else {
				EscapeJsonString(fkyaml::node::serialize(pair.first), out);
			}
			out += ':';
			EmitJson(pair.second, out);
		}
		out += '}';
		return;
	}
	if (node.is_sequence()) {
		out += '[';
		bool first = true;
		for (auto &child : node.as_seq()) {
			if (!first) {
				out += ',';
			}
			first = false;
			EmitJson(child, out);
		}
		out += ']';
		return;
	}
	if (node.is_null()) {
		out += "null";
		return;
	}
	if (node.is_boolean()) {
		out += node.as_bool() ? "true" : "false";
		return;
	}
	if (node.is_integer()) {
		out += std::to_string(node.as_int());
		return;
	}
	if (node.is_float_number()) {
		out += StringUtil::Format("%.17g", node.as_float());
		return;
	}
	// Everything else is a string scalar.
	EscapeJsonString(node.as_str(), out);
}

} // namespace

bool LooksLikeYaml(const string &text) {
	for (auto c : text) {
		if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
			continue;
		}
		return c != '{';
	}
	// Empty input: let the JSON parser produce the "not valid JSON" message.
	return false;
}

string YamlToJson(const string &yaml_text, const string &context) {
	try {
		auto root = fkyaml::node::deserialize(yaml_text.begin(), yaml_text.end());
		string json;
		json.reserve(yaml_text.size() * 2);
		EmitJson(root, json);
		return json;
	} catch (const fkyaml::exception &ex) {
		throw InvalidInputException("ossie_load: %s is not valid YAML: %s", context, ex.what());
	}
}

} // namespace ossie
} // namespace duckdb
