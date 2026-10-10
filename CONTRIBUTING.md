# Contributing

Thanks for looking. Issues and pull requests are welcome. Cases where a model is
misinterpreted by the extension are helpful.

## Build

Both submodules must be checked out before any `make` target resolves.

```sh
git submodule update --init --recursive   # duckdb v1.5.6, extension-ci-tools v1.5.6, fkYAML
make                                      # first build compiles DuckDB from source; slow, then incremental
make test                                 # SQLLogicTests in test/sql/
```

There is no vcpkg step and nothing is fetched at build time.
The build rules can be found in `extension-ci-tools`.

Submodules are pinned by recorded commit.
DuckDB's parser and planner internals are not a stable API so they are pinned.

A single test file goes to the unittest binary directly:

```sh
./build/release/test/unittest test/sql/ossie_grain.test
```

Double check the assertion count is what you expect.

## Rules

Changes should avoid violating these rules:

**Never return a wrong number.** If a query is ambiguous for the given model, refuse
with an error detailing why. The primary consumer is an AI agent that cannot check
the number it receives, so a plausible wrong answer is worse than an error. Every refusal should
be document in [docs/limitations.md](docs/limitations.md).

**Avoid building SQL by string concatenation.** Use `ParsedExpression` tree from load to
emit; quoting and precedence come from DuckDB's printer.

**Where possible, fail at load time instead of query time.** Malformed expressions, query-valued `source:`, dangling
relationship endpoints and non-`ANSI_SQL`-only expressions are all `ossie_load` errors.

**Use useful error text.** An agent reads a refusal and retries, so messages name the
offending object and tests assert on the message, not merely that something threw.

## Tests

| layer | files | catches |
|---|---|---|
| load and validation | `ossie_load`, `ossie_validate` | malformed models, structural errors |
| description | `ossie_describe`, `ossie_relationships` | vocabulary surface, cardinality |
| golden SQL | `ossie_compile`, `ossie_grain` | compiler output and refusals |
| conformance | `ossie_conformance` | disagreements with other implementers |
| differential | `ossie_differential` | **wrong numbers** |

Only the differential layer catches an incorrect *number* — a golden-file test passes just as
happily on SQL that is valid and wrong. Those tests generate TPC-DS data and diff generated SQL
against hand-written equivalents with symmetric `EXCEPT`. **Add one whenever you touch grain, join
planning, or fan-out.**

The conformance fixtures are models written by other Ossie implementers, copied verbatim. They are
the only fixtures that can detect a divergence between our reading of the format and anyone else's,
because every other fixture here was written alongside the compiler and shares its assumptions.

## Formatting

Matched to DuckDB's and enforced in CI. `clang_format` must be **exactly 11.0.1** — a newer one
disagrees with DuckDB's config and CI will fail on a tree your local tool called clean.

```sh
python3 -m venv build/fmtvenv
build/fmtvenv/bin/pip install "black>=24" "clang_format==11.0.1" cmake-format
PATH="$PWD/build/fmtvenv/bin:$PATH" make format-check   # what CI runs; non-mutating
PATH="$PWD/build/fmtvenv/bin:$PATH" make format         # rewrites in place
```

New `.cpp` files must be added to `EXTENSION_SOURCES` in `CMakeLists.txt`.

## Pull requests

Submit a pull request and make sure the `CI gate` check passes. That job
runs the nine-platform build, the MCP round trip test, the artifact-load check test and quality
checks.

Before pushing, you can run the `CI gate` job locally to test:

```sh
make && make test
PATH="$PWD/build/fmtvenv/bin:$PATH" make format-check
./scripts/full_functionality_check.sh
python3 scripts/mcp_check.py
```

## Where to read more

- [docs/architecture.md](docs/architecture.md) — how the compiler works and how to work on it
- [docs/limitations.md](docs/limitations.md) — refusals and the reasoning behind them
- [docs/UPDATING.md](docs/UPDATING.md) — moving to a new DuckDB version

## Licence

Contributions are accepted under the [MIT licence](LICENSE)
