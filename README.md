# duckdb_ossie

A DuckDB extension that reads [Apache Ossie](https://github.com/apache/ossie) (incubating)
semantic model files and answers semantic queries against the tables they describe — locally,
from a single binary, with no infrastructure.

Ossie is a vendor-neutral file format for semantic models: `datasets` bound to physical tables,
`fields`, declared `relationships`, and `metrics` written as aggregate expressions. It describes
data in place and never transforms it. What the format deliberately leaves open is the *query*
those definitions go into — the joins, the `GROUP BY`, and the grain — because those depend on the
question being asked, and the file does not know the question.

Every shipped Ossie implementation today is a *converter*, translating definitions into some
vendor's semantic layer so that vendor's engine can run them. This extension is the missing engine.

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
from. It can reach nothing but the model's own vocabulary: filters are allowlisted, subqueries are
refused outright, and no argument lets a caller widen that.

## Building

No vcpkg or third-party dependencies; JSON parsing uses the yyjson DuckDB already vendors.

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

MIT. See [LICENSE](LICENSE).
