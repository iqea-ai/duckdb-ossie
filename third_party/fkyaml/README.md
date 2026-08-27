# fkYAML (vendored)

`node.hpp` is the fkYAML single-header release, verbatim and unmodified.

- Version: **v0.4.4**
- Upstream: https://github.com/fktn-k/fkYAML
- License: MIT (SPDX-License-Identifier at the top of the header)

## Why this is here

DuckDB vendors yyjson but no YAML parser, and the Ossie ecosystem publishes models as YAML: the
Apache Ossie repository contains 51 YAML files and 6 JSON, and every one of the JSON files is a
schema or a converter's output rather than a model. Without a YAML reader this extension cannot open
a single real-world Ossie model.

It is used only to convert YAML into the JSON text the existing parser already accepts, so the
27 load-time validation guards and the compiler are unaffected by the file's serialization.

## Updating

Replace the file with a newer single-header release and update the version above. Do not edit it
locally; if a change is needed, send it upstream.
