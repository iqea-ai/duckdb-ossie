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

## Alternatives that were considered and rejected

**The `yaml` community extension.** One exists and works on v1.5.5 (`read_yaml`, `from_yaml`,
`parse_yaml`, ...). Rejected because it would make reading a model file depend on the user having
installed a second community extension: `description.yml` would need `install_notes`, two
independently versioned extensions would have to stay compatible, and CI and the registry build
would both need it present. That is the same support burden duckdb-snowflake carries for its ADBC
driver. Today this extension has no runtime dependencies at all, which is worth protecting.
Mechanically it is also a worse path: `from_yaml` returns a DuckDB value, so the route becomes
YAML -> DuckDB type -> JSON text -> yyjson, which is more conversion surface, not less.

**A vcpkg dependency.** Rejected because `CMakeLists.txt` deliberately keeps vcpkg off the critical
path so the build is reproducible from a bare checkout. A vcpkg dependency has to resolve on all ten
CI architectures including three WASM targets and MinGW, and `vcpkg.json` already points
`overlay-triplets` at a directory that does not exist upstream.

**A hand-written YAML subset parser.** Rejected because YAML's edge cases -- block scalars, anchors,
quoting, indentation -- are exactly where a subtle misread silently yields the wrong model. A few
hundred lines we maintain is worse than nine thousand we do not, in this particular place.

## Updating

Replace the file with a newer single-header release and update the version above. Do not edit it
locally; if a change is needed, send it upstream.
