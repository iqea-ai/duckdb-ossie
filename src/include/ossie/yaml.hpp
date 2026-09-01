#pragma once

#include "duckdb/common/string.hpp"

namespace duckdb {
namespace ossie {

//! True if `text` looks like YAML rather than JSON. A JSON document's first non-whitespace
//! character is '{' (an Ossie model is always an object); anything else is treated as YAML.
bool LooksLikeYaml(const string &text);

//! Converts a YAML document into equivalent JSON text. Throws InvalidInputException if it does not
//! parse. The result is fed to the ordinary JSON parser, so a model's serialization never reaches
//! validation or the compiler.
string YamlToJson(const string &yaml_text, const string &context);

} // namespace ossie
} // namespace duckdb
