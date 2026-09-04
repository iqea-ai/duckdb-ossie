# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The version recorded here is the **extension version** (what the
[community-extensions descriptor](https://github.com/duckdb/community-extensions/blob/main/extensions/ossie/description.yml)
pins). The DuckDB version each release targets is noted separately.

## [Unreleased]

## [0.1.1] - 2026-09-03

Targets DuckDB **v1.5.5**.

Both fixes came from writing an MCP client and driving the published server the way Claude Desktop
does. Neither the test suite nor the end-to-end functionality check covered that path.

### Fixed
- `examples/server.sql` never loaded the extension it demonstrates. It loaded `duckdb_mcp` but not
  `ossie`, and called `dsdgen` without loading `tpcds`, so it only worked from a build tree where
  both are statically linked. Anyone following the README after installing from the registry hit
  `Catalog Error: Table Function with name ossie_load does not exist!` on the first statement that
  mattered. All three extensions are now installed and loaded explicitly

### Changed
- Corrected a security claim. The documentation stated that an agent connected to the MCP server
  "can reach nothing but the model's own vocabulary". `duckdb_mcp` also publishes generic `query`,
  `export`, `list_tables` and `describe` tools and currently offers no way to disable them, so an
  agent can run arbitrary SQL against the database the server was started against — verified by
  reading a table the semantic model never declares. The allowlisted-filter and no-subquery
  guarantees are real but apply to `semantic_query`, not to the connection. README, the registry
  descriptor, and `examples/server.sql` now say so

### Added
- `scripts/mcp_check.py`, an end-to-end MCP check that speaks JSON-RPC over stdio to the published
  server and asserts the full round trip: handshake, tool discovery, vocabulary discovery, real rows
  from `semantic_query`, and a multi-grain refusal arriving as a readable error

## [0.1.0] - 2026-08-27

Targets DuckDB **v1.5.5**.

First release. Ossie is a vendor-neutral file format for semantic models, and every shipped
implementation of it is a *converter* — it translates definitions into some vendor's semantic layer
so that vendor's engine can run them. This extension is an engine: it compiles a semantic request
against the model and runs the SQL on DuckDB.

### Added
- `ossie_load(path, ...)` parses and validates an Ossie model, in either YAML or JSON, holding it in the database's
  `ObjectCache` rather than per connection so a model loaded by an init script is visible to an MCP
  server answering on its own connection. Named arguments: `rebind` remaps warehouse-qualified
  source prefixes onto the local catalog, `validate_sources` requires every dataset's table to
  exist (reporting all failures at once), and `allow_filter_functions` sets the filter policy at
  load so a caller supplying filters cannot widen its own policy
- `ossie_query(metrics, dimensions, filters)` compiles the request and executes it. The statement is
  returned from `bind_replace` as a `SubqueryRef`, which runs before binding — so DuckDB plans and
  optimizes the generated query as though it had been typed by hand, and join reordering, filter
  pushdown, and projection pruning are inherited rather than reimplemented
- `ossie_compile(metrics, dimensions, filters)` returns the same SQL as text without running it,
  for use where DuckDB is not the executor. It runs the identical compiler, so the SQL you inspect
  is the SQL that would run
- `ossie_datasets()`, `ossie_fields()`, `ossie_metrics()`, and `ossie_relationships()` expose the
  model's vocabulary, including `ai_context` synonyms so an agent can discover names, and each
  relationship's cardinality derived from the endpoints' declared keys
- Grain analysis: each aggregate's grain is the dataset that functionally determines the others it
  references, and that dataset becomes the root of the join tree. Every aggregate in a query must
  agree on one grain
- Join planning by breadth-first search over the relationship graph, with two refusals: reaching a
  required dataset by a second, non-tree edge (two routes that could produce different numbers),
  and any edge that repeats rows of the grain and would inflate every aggregate
- Filter predicates are compiled into `WHERE` or `HAVING` depending on whether they contain an
  aggregate, and are allowlisted by node type — operators always, named function calls only under
  `allow_filter_functions`, subqueries never
- Expressions are parsed into `ParsedExpression` trees at load and rewritten as trees through to
  emission; no SQL is ever built by string concatenation
- `examples/server.sql` publishes a model to an AI agent over MCP via the `duckdb_mcp` community
  extension, exposing a `semantic_query` tool plus `metrics` and `dimensions` resources
- YAML and JSON are both read natively. The Ossie ecosystem publishes models as YAML — the
  apache/ossie repository holds 51 YAML files to 6 JSON, and every one of those JSON files is a
  schema or a converter's output rather than a model. DuckDB vendors no YAML parser, so fkYAML is
  vendored as a single unmodified header and YAML is converted to JSON before parsing, which leaves
  every validation guard and the compiler unaware of how a model was serialized
- Conformance fixtures: five models taken verbatim from apache/ossie, plus the official
  `core-spec/osi-schema.json`. Every other fixture here was written alongside the compiler and so
  shares its assumptions; these are the only ones that can detect a divergence between this
  implementation's reading of the format and anyone else's. Four of the five load, and the fifth is
  refused correctly because it carries only `DATABRICKS` expressions
- `scripts/full_functionality_check.sh`, an end-to-end check driven through a stock DuckDB CLI
  against the loadable artifact rather than the build tree

### Known limitations
Every refusal and its reasoning is recorded in [docs/limitations.md](docs/limitations.md). The ones
most likely to affect you:
- **`ANSI_SQL` expressions only.** A field or metric carrying only a `SNOWFLAKE`, `DATABRICKS`, or
  `MDX` expression fails at load rather than being guessed at
- **Metrics spanning more than one grain are refused** rather than answered wrongly. Computing them
  correctly needs one aggregate per grain joined on shared dimensions, which is not implemented
- **All joins are emitted as `INNER`.** Ossie declares a relationship's endpoints but not its join
  type, so a fact row with a `NULL` foreign key is dropped once its dimension is joined, and a
  metric total can shrink when a dimension is added. There is no override yet
- **One semantic model per file.** The format permits an array; metrics are addressed by bare name
  with no way to say which model is meant
- **A column named by a model expression but not declared as a field is passed through** to DuckDB's
  binder rather than refused, because the format does not require the declaration and real models
  rely on that. The cost is that a typo in a model expression surfaces when a query touches it
  instead of at load
- **No row cap.** `ossie_query` takes metrics, dimensions, and filters and nothing else, so a
  high-cardinality dimension returns every row

[Unreleased]: https://github.com/iqea-ai/duckdb-ossie/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/iqea-ai/duckdb-ossie/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/iqea-ai/duckdb-ossie/releases/tag/v0.1.0
