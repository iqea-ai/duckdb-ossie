# Contributing

Thanks for looking. Issues and pull requests are welcome — bug reports especially, and most of all
a model that this extension reads wrongly.

## Getting a build

Both submodules must be checked out before any `make` target resolves.

```sh
git submodule update --init --recursive   # duckdb v1.5.5, extension-ci-tools v1.5.5, fkYAML
make                                      # first build compiles DuckDB from source; slow, then incremental
make test                                 # SQLLogicTests in test/sql/
```

There is no vcpkg step and nothing is fetched at build time. JSON parsing uses the yyjson DuckDB
already vendors, and YAML is a single vendored header, so a bare checkout builds.

Skip the submodule step and `make` fails on its first line, without saying why:

```
Makefile:8: extension-ci-tools/makefiles/duckdb_extension.Makefile: No such file or directory
```

`extension-ci-tools` supplies the actual build rules — the `Makefile` here is a two-line wrapper
around it.

Submodules are pinned by recorded commit and `.gitmodules` carries no `branch` entry, so
`git submodule update` restores exactly those commits. That matters more than it looks:
`extension-ci-tools` publishes no tags at all, only moving version branches, so `v1.5.5` there is a
branch whose head can change. The recorded commit is what makes a checkout reproducible.

DuckDB's parser and planner internals are not a stable API; the pin is deliberate, and a version
bump is expected to require code changes.

A single test file goes to the unittest binary directly:

```sh
./build/release/test/unittest test/sql/ossie_grain.test
```

One false green to know about: a path matching no registered test prints `No tests ran` and still
**exits 0**. Check the assertion count, not just the exit code.

## The rules that outrank features

These come first, and a change that violates one is a bug regardless of what the tests say.

**Never return a wrong number.** Where the model or the request underdetermines the query, refuse
with an error naming the offending object. The primary consumer is an AI agent that cannot check
the number it receives, so a plausible wrong answer is worse than an error. Every refusal belongs in
[docs/limitations.md](docs/limitations.md).

**Never build SQL by string concatenation.** Everything is a `ParsedExpression` tree from load to
emit; quoting and precedence come from DuckDB's printer.

**Fail at load time, not query time.** Malformed expressions, query-valued `source:`, dangling
relationship endpoints and non-`ANSI_SQL`-only expressions are all `ossie_load` errors.

**Error text is part of the interface.** An agent reads a refusal and retries, so messages name the
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

`main` is protected: changes go through a pull request, and the `CI gate` check must pass. That job
aggregates the nine-platform build, the MCP round trip, the artifact-load check and the quality
checks, so a green `CI gate` means all of them passed.

Before pushing, the same gate locally:

```sh
make && make test
PATH="$PWD/build/fmtvenv/bin:$PATH" make format-check
./scripts/full_functionality_check.sh
python3 scripts/mcp_check.py
```

Keep the commit message explaining *why*, not what — the diff already says what.

## Where to read more

- [docs/architecture.md](docs/architecture.md) — how the compiler works and how to work on it
- [docs/limitations.md](docs/limitations.md) — every refusal and the reasoning behind it
- [docs/UPDATING.md](docs/UPDATING.md) — moving to a new DuckDB version

## Licence

Contributions are accepted under the [MIT licence](LICENSE), the same terms as the project.
