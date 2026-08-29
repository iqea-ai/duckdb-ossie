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

**The `yaml` community extension.** Tried on branch `experiment/yaml-extension` and rejected on
correctness, not preference. `read_yaml` infers YAML 1.1 types, so it reads these as booleans:

    a: t     -> true        e: yes   -> true
    b: y     -> true        f: on    -> true
    c: n     -> false       g: off   -> false
    d: no    -> false       h: "t"   -> true      <- quoted, and still a boolean
    j: 012   -> 12 (TINYINT, leading zero lost)

The `h` case is decisive: an explicitly quoted string still comes back `BOOLEAN`, verified with
`typeof(y.h)`, so a model author has no way to protect a value. A dataset, field, or metric named
`t`, `y`, `n`, `no`, `yes`, `on`, or `off` would be silently renamed to `true` or `false`. fkYAML
implements YAML 1.2, where only `true`/`false` are booleans, and keeps every one of those as a
string -- see test/sql/ossie_yaml.test.

Two further costs, either of which would have been reason enough on its own: the extension does not
autoload (`loaded=false` until an explicit `LOAD`), so a user would need a second install before any
YAML model would open, requiring `install_notes` in the descriptor; and the conversion path becomes
YAML -> DuckDB STRUCT -> JSON text -> yyjson, which is one representation more than we have now.

**A vcpkg dependency.** Tried on branch `experiment/yaml-vcpkg`, which is the same library at the
same nominal version, and CI rejected it for two concrete reasons:

- **The version is not ours to choose.** The vcpkg port manifest says 0.4.4, but the build resolved
  `fkyaml::v0_4_2` -- the DuckDB extension ecosystem pins its own vcpkg baseline, and that baseline's
  fkYAML predates the `as_map`/`as_str`/`as_int` accessors this code uses. Linux arm64 failed with
  `has no member named 'as_map'`. Tracking the ecosystem's baseline means the parser version can move
  under us on any bump.
- **`find_package(fkYAML CONFIG REQUIRED)` breaks the code-quality job**, which configures CMake
  without a vcpkg toolchain: `Could not find a package configuration file provided by "fkYAML"`.

Both are surmountable, but neither is the drop-in it appeared to be, and vendoring costs nothing at
build time.

**A hand-written YAML subset parser.** Rejected because YAML's edge cases -- block scalars, anchors,
quoting, indentation -- are exactly where a subtle misread silently yields the wrong model. A few
hundred lines we maintain is worse than nine thousand we do not, in this particular place.

## Updating

Replace the file with a newer single-header release and update the version above. Do not edit it
locally; if a change is needed, send it upstream.
