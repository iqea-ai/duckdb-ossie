# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

The version recorded here is the **extension version** (what the
[community-extensions descriptor](https://github.com/duckdb/community-extensions/blob/main/extensions/ossie/description.yml)
pins). The DuckDB version each release targets is noted separately.

## [Unreleased]

## [0.1.0] - 2026-08-27

Targets DuckDB **v1.5.5**.

First release. Ossie is a vendor-neutral file format for semantic models, and every shipped
implementation of it is a *converter* — it translates definitions into some vendor's semantic layer
so that vendor's engine can run them. This extension is an engine: it compiles a semantic request
against the model and runs the SQL on DuckDB.

### Added
- `ossie_load(path, ...)` parses and validates an Ossie JSON model, holding it in the database's
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
- `scripts/full_functionality_check.sh`, an end-to-end check driven through a stock DuckDB CLI
  against the loadable artifact rather than the build tree

### Known limitations
Every refusal and its reasoning is recorded in [docs/limitations.md](docs/limitations.md). The ones
most likely to affect you:
- **JSON models only.** The Ossie ecosystem publishes models as YAML; convert before loading
- **`ANSI_SQL` expressions only.** A field or metric carrying only a `SNOWFLAKE`, `DATABRICKS`, or
  `MDX` expression fails at load rather than being guessed at
- **Metrics spanning more than one grain are refused** rather than answered wrongly. Computing them
  correctly needs one aggregate per grain joined on shared dimensions, which is not implemented
- **All joins are emitted as `INNER`.** Ossie declares a relationship's endpoints but not its join
  type, so a fact row with a `NULL` foreign key is dropped once its dimension is joined, and a
  metric total can shrink when a dimension is added. There is no override yet
- **One semantic model per file.** The format permits an array; metrics are addressed by bare name
  with no way to say which model is meant

[Unreleased]: https://github.com/iqea-ai/duckdb-ossie/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/iqea-ai/duckdb-ossie/releases/tag/v0.1.0
