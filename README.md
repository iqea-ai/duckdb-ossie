# duckdb_ossie

[![Main Extension Distribution Pipeline](https://github.com/iqea-ai/duckdb-ossie/actions/workflows/MainDistributionPipeline.yml/badge.svg?branch=main)](https://github.com/iqea-ai/duckdb-ossie/actions/workflows/MainDistributionPipeline.yml)
[![Community Extension](https://img.shields.io/badge/community--extensions-ossie-blue)](https://github.com/duckdb/community-extensions/blob/main/extensions/ossie/description.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

The [Apache Ossie](https://github.com/apache/ossie) (incubating) reference implementation for
DuckDB. It reads Ossie semantic model files — YAML or JSON — and answers semantic queries against
the tables in DuckDB, from a single binary, with no infrastructure. This extension also exposes the
semantic layer via MCP.

**This extension works with DuckDB v1.5.5.**

Ossie is a vendor-neutral file format for semantic models: `datasets` bound to physical tables,
`fields`, declared `relationships`, and `metrics` written as aggregate expressions. It describes
data in place and never transforms it. What the format deliberately leaves open is the *query*
those definitions go into — the joins, the `GROUP BY`, and the grain — because those depend on the
question being asked, and the file does not know the question.

**Every other shipped Ossie implementation is a converter**, translating definitions into some
vendor's semantic layer so that vendor's engine can run them. This one executes the query itself:
it plans the joins, derives the grain, and emits SQL that DuckDB runs. It is the first Ossie
implementation that answers a question rather than translating one.

### Conformance

Claims about a file format should be checkable, so:

- Models are validated against the format's own [`core-spec/osi-schema.json`](https://github.com/apache/ossie/blob/main/core-spec/osi-schema.json)
- The suite includes five models published by *other* Ossie implementers — Databricks, GoodData,
  NVIDIA, Omni and OrionBelt — vendored verbatim from `apache/ossie`. Four load and answer queries.
  The fifth carries only `DATABRICKS` expressions, which this extension does not execute
- Generated SQL is checked against hand-written TPC-DS SQL at `sf=1` across every metric and every
  dimension, so correctness is measured against the numbers rather than against our own output

What is not yet supported is listed in [docs/limitations.md](docs/limitations.md): `ANSI_SQL`
expressions only, one semantic model per file, table-backed sources only, and metrics that span
more than one grain are refused rather than answered wrongly.

```sql
CALL ossie_load('model.json', rebind => MAP{'tpcds.public': 'tpcds.main'});

SELECT * FROM ossie_metrics();                       -- discover the vocabulary
SELECT * FROM ossie_query(['total_sales'],           -- ask a question
                          ['item.i_brand'],
                          ['date_dim.d_year = 2001']);
```

```
┌───────────────────┬───────────────┐
│   item.i_brand    │  total_sales  │
├───────────────────┼───────────────┤
│ amalgimporto #2   │     709288.31 │
│ edu packamalg #2  │     565064.01 │
└───────────────────┴───────────────┘
```

## Why refusals matter

The primary consumer here is an AI agent, which cannot check the number it gets back. A plausible
wrong answer is therefore worse than an error, and the extension is built to refuse rather than
guess. Ask for `store_productivity` — which sums a fact column and a dimension column — and you get:

```
ossie: these metrics aggregate at 2 different grains (store, store_sales).
Computing them together needs one aggregate per grain, which is not supported yet
```

rather than a number roughly a thousand times too large. [docs/limitations.md](docs/limitations.md)
records every such refusal and why it exists.

## Functions

| Function | Purpose |
|---|---|
| `ossie_load(path, ...)` | Parse and validate a model. Named args: `rebind`, `validate_sources`, `allow_filter_functions` |
| `ossie_datasets()` | Datasets, their sources, keys, and whether each source resolves |
| `ossie_fields()` | Fields with datatype, `is_time`, `is_computed`, expression, synonyms |
| `ossie_metrics()` | Metrics with datatype, expression, synonyms |
| `ossie_relationships()` | Relationships with derived cardinality |
| `ossie_query(metrics, dimensions, filters)` | Compile the request and execute it |
| `ossie_compile(metrics, dimensions, filters)` | Return the SQL without running it |

`rebind` remaps warehouse-qualified prefixes onto the local catalog: a model saying
`tpcds.public.store_sales` needs `rebind => MAP{'tpcds.public': 'tpcds.main'}` to find local tables.

`ossie_compile` is a first-class output, not a debug aid — it is the useful surface wherever DuckDB
is not the executor. It runs the identical compiler `ossie_query` does, so the SQL you inspect is
the SQL that would run.

## Serving a model to an AI agent

[examples/server.sql](examples/server.sql) publishes the model over MCP:

```sh
duckdb -unsigned -init examples/server.sql
```

Point Claude Desktop at it:

```json
{"mcpServers": {"ossie": {"command": "duckdb",
                          "args": ["-unsigned", "-init", "/abs/path/to/server.sql"]}}}
```

The agent gets a `semantic_query` tool plus `metrics` and `dimensions` resources to discover names
from. Within that tool it can reach nothing but the model's own vocabulary: filters are allowlisted,
subqueries are refused outright, and no argument lets a caller widen that.

> **Read this before pointing an agent at real data.** `duckdb_mcp` also publishes its own generic
> tools — `query`, `export`, `list_tables`, `describe` — and there is currently no option to turn
> them off. An agent connected to this server can therefore run arbitrary SQL and read any table in
> the database, including tables the semantic model never declares. The guarantees above apply to
> `semantic_query`, not to the connection as a whole. Until that is configurable, treat the server as
> having full access to whatever database it is started against, and point it only at data you are
> willing to expose.

## Building

No vcpkg and no build-time dependency fetching. JSON parsing uses the yyjson DuckDB already vendors;
YAML support is a single vendored header ([third_party/fkyaml](third_party/fkyaml), MIT).

```sh
git submodule update --init --recursive
make
```

Submodules are pinned by recorded commit (DuckDB `v1.5.5`). DuckDB's parser and planner internals
are not a stable API, so the pin is deliberate and upgrades are expected to need work. The first
build compiles DuckDB from source and takes a while; later builds are incremental.

```sh
make test
```

For the architecture and how to work on the compiler, see
[docs/architecture.md](docs/architecture.md).

## License

MIT. See [LICENSE](LICENSE), and [NOTICE](NOTICE) for third-party content redistributed with this
repository — the fkYAML submodule (MIT) and the Apache-2.0 licensed Ossie models used as conformance
test data.

This project is an independent implementation that reads the Apache Ossie file format. It is not
affiliated with, endorsed by, or an official product of the Apache Software Foundation. "Apache
Ossie" and "Apache" are trademarks of the Apache Software Foundation.
