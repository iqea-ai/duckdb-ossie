# duckdb_ossie — Implementation Plan

A DuckDB extension that reads Apache Ossie semantic model files and answers
semantic queries against the tables they describe, exposed to AI agents over MCP.

This document is the working spec. It is written to be handed to an agentic coding
tool. Read the whole thing before writing code — the invariants and non-goals matter
more than the file layout.

---

## 1. Context

### What Apache Ossie is

Apache Ossie (incubating, formerly Open Semantic Interchange) is a vendor-neutral
**file format** for semantic models. A model is a YAML or JSON document with four
building blocks:

- `datasets` — logical entities bound to physical tables via `source: db.schema.table`
- `fields` — columns, or SQL expressions over columns, tagged with `datatype` and
  `dimension.is_time`
- `relationships` — declared foreign-key edges: `from`/`to` datasets plus
  `from_columns`/`to_columns`
- `metrics` — aggregate expressions written against qualified names, e.g.
  `SUM(store_sales.ss_ext_sales_price)`

Plus `ai_context` blocks (synonyms, instructions) scattered throughout, aimed at LLM
consumers.

Reference model to develop against:
`https://github.com/apache/ossie/blob/main/examples/tpcds_semantic_model.yaml`

### The critical fact

**Ossie describes data in place. It never transforms data.** `source:` points at a
table that already exists. The `expression` fields exist so the user does not have to
reshape anything.

### The gap this extension fills

The spec defines leaf expressions but not the query they go into. Search the TPC-DS
example for `GROUP BY`, `WHERE`, or `JOIN` — zero hits. Those depend on the question
being asked, and the file does not know the question.

No engine reads Ossie natively today. The shipped reference implementations are
*converters* (Ossie→dbt MetricFlow, Ossie→Polaris, Ossie→Snowflake semantic views) that
translate definitions into a vendor format so that vendor's engine can run them. A
standardized semantic query language with a reference compiler is under discussion in
the Ossie community but not shipped.

This extension is that missing engine: embedded, single-binary, zero-infrastructure.

### Why a compiler and not a template

The same metric definition produces different SQL depending on what was requested:

```sql
-- total_sales, no dimensions
SELECT SUM(ss_ext_sales_price) FROM store_sales;

-- total_sales by item.i_brand
SELECT item.i_brand, SUM(store_sales.ss_ext_sales_price)
FROM store_sales JOIN item ON store_sales.ss_item_sk = item.i_item_sk
GROUP BY item.i_brand;
```

The fragment from the file is byte-identical in both. Everything else was derived at
request time from the relationships graph. The correct join set is a function of the
requested fields — which is why the decision cannot be made once at load time.

---

## 2. Invariants

These hold in every phase. Violating one is a bug regardless of what the tests say.

1. **Never return a wrong number.** Refusing with a clear error is always correct
   behavior. An agent querying this extension cannot check the answer, so a plausible
   wrong number is worse than a failure. This outranks feature coverage.

2. **Never build SQL by string concatenation.** Parse fragments into
   `ParsedExpression` trees at load time, rewrite the trees, emit from the tree.
   String splicing produces quoting bugs, precedence bugs, and injection.

3. **Fail at load time, not query time.** Malformed expressions, unresolvable
   `source:` names, and dangling relationship endpoints are `ossie_load` errors.

4. **`ossie_compile()` is a first-class output, not a debug aid.** It makes the
   extension useful where DuckDB is not the executor, and it makes the test suite cheap
   (golden files, no database needed).

5. **The model is the authorization boundary.** A name not in the model does not
   resolve. Never provide an escape hatch that lets a caller reference an unmodeled
   column.

---

## 3. Non-goals

Explicitly out of scope. Do not build these speculatively.

- Writing, mutating, or validating Ossie models beyond schema conformance
- Converting Ossie to other semantic formats (that space is well covered)
- Any ontology / OWL / RDF layer — the Ossie ontology layer is roadmap, not spec
- Query result caching
- Ossie `custom_extensions` vendor blocks — parse and ignore
- Natural-language → metric-name mapping. The agent does that; the extension exposes
  vocabulary and accepts exact names.

---

## 4. Architecture

```
ossie_load(path)
  └─ parse JSON/YAML → validate → build Model
       ├─ datasets, fields, relationships, metrics
       ├─ ParsedExpression tree per field and metric  (parsed once, here)
       ├─ relationship graph with per-edge cardinality flags
       └─ stored in ClientContextState

ossie_query(metrics, dimensions, filters)      ossie_compile(same args)
  └─ Compiler                                    └─ Compiler
       ├─ resolve names → required datasets           └─ stmt->ToString()
       ├─ plan joins over the graph
       ├─ grain check (refuse or split)
       ├─ rewrite expression trees to aliases
       └─ assemble SelectNode
            └─ bind_replace → SubqueryRef
```

### Compiler stages

**Stage 1 — Resolve.** Map each requested identifier to a metric or field, and record
the owning dataset. Unresolvable names error here with a message listing near matches.
Output: the set of required datasets.

**Stage 2 — Plan joins.** Find the minimal connected subgraph of `relationships`
covering the required datasets. For a star schema this is a walk outward from the fact
table. In general it is a Steiner tree. If more than one path connects the same pair of
datasets, **refuse** — do not pick.

**Stage 3 — Grain check.** For each edge in the join tree, determine cardinality: the
edge is many-to-one if `to_columns` matches the target dataset's `primary_key` **or any
entry in `unique_keys`**. Check both — in the TPC-DS example `store` declares
`primary_key: [s_store_sk]` but `unique_keys: [[s_store_id]]`, and the relationship
joins on `s_store_sk`.

If a requested metric aggregates a column from a dataset that fans out relative to the
query grain, either refuse (phase 3a/3b) or split into a per-grain subquery (phase 3c).

**Stage 4 — Rewrite.** Walk each metric/field `ParsedExpression` tree. For every
`ColumnRefExpression` whose qualifier names a dataset, rebind to the bound table alias.
If the referenced field is itself computed, inline a copy of its expression tree.
`ParsedExpressionIterator::EnumerateChildren` gives the traversal.

**Stage 5 — Emit.** Build `SelectNode` with `select_list`, nested `JoinRef` in
`from_table`, `where_clause`, `groups.group_expressions`, and
`aggregate_handling = AggregateHandling::STANDARD_HANDLING`.

- `ossie_compile` → `stmt->ToString()`
- `ossie_query` → return `make_uniq<SubqueryRef>(std::move(stmt))` from `bind_replace`

`bind_replace` runs before binding, so DuckDB plans and optimizes the generated query as
if the user typed it. Join reordering, filter pushdown, and projection pruning come free
— do not reimplement them.

### Worked example: fan-out

`store_productivity` is defined as:

```
SUM(store_sales.ss_ext_sales_price) / NULLIF(SUM(store.s_number_employees), 0)
```

Joining `store` to `store_sales` replicates `s_number_employees` once per sale line.
Naive evaluation returns a number ~1000× too large. The correct shape computes each
aggregate at its own grain:

```sql
WITH sales AS (
  SELECT store.s_state, SUM(store_sales.ss_ext_sales_price) AS num
  FROM store_sales JOIN store ON store_sales.ss_store_sk = store.s_store_sk
  GROUP BY store.s_state
),
staff AS (
  SELECT s_state, SUM(s_number_employees) AS den
  FROM store
  GROUP BY s_state
)
SELECT sales.s_state, sales.num / NULLIF(staff.den, 0) AS store_productivity
FROM sales JOIN staff USING (s_state)
```

Phases 3a/3b refuse this query. Phase 3c produces the above.

---

## 5. Public interface

### Loading

```sql
CALL ossie_load('model.json');
CALL ossie_load('model.json', rebind => MAP{'tpcds.public': 'tpcds.main'});
```

`rebind` remaps warehouse-qualified `source:` prefixes onto the local catalog. Without
it, TPC-DS models referencing `tpcds.public.*` will not resolve against a local DuckDB
catalog. Required in practice — implement it in phase 0.

### Introspection

```sql
SELECT * FROM ossie_datasets();       -- name, source, primary_key, description, synonyms
SELECT * FROM ossie_fields();         -- dataset, name, datatype, is_time, expression, synonyms
SELECT * FROM ossie_metrics();        -- name, datatype, expression, description, synonyms
SELECT * FROM ossie_relationships();  -- name, from, to, from_columns, to_columns, cardinality
```

These are how an agent discovers vocabulary. They are not scaffolding.

### Querying

```sql
SELECT * FROM ossie_query(['total_sales'], ['item.i_brand'], ['date_dim.d_year = 2001']);
SELECT ossie_compile(['total_sales'], ['item.i_brand'], ['date_dim.d_year = 2001']);
```

Dimensions are `dataset.field`. Filters are SQL predicate strings over qualified names;
parse and validate them, never splice them raw.

### MCP publication

```sql
INSTALL duckdb_mcp FROM community; LOAD duckdb_mcp;

PRAGMA mcp_publish_tool(
  'semantic_query',
  'Query the semantic model by metric and dimension names. Call list_metrics first.',
  'SELECT * FROM ossie_query($metrics, $dimensions, $filters)',
  '{"metrics":{"type":"string"},"dimensions":{"type":"string"},"filters":{"type":"string"}}',
  '["metrics"]'
);
PRAGMA mcp_publish_query('vocabulary', 'SELECT * FROM ossie_metrics()');
PRAGMA mcp_server_start('stdio');
```

Verify these signatures against the current `duckdb_mcp` docs before relying on them;
the extension is young and the pragma arguments may have shifted.

---

## 6. Phases

Each phase ends with something demoable. Do not start the next phase until the exit
criteria pass in CI.

### Phase 0 — Parse, validate, introspect

Build the extension skeleton from the DuckDB community extension template, pinned to a
specific DuckDB version. JSON models only.

- `ossie_load` with `rebind`
- Full model struct; every field and metric expression parsed via
  `Parser::ParseExpressionList` **at load time** and retained as a tree
- Validation: `source:` resolvable, relationship endpoints exist, referenced columns
  exist in their dataset, expressions parse
- Model stored in `ClientContextState`
- Four introspection table functions

**Exit:** `SELECT * FROM ossie_metrics()` on the TPC-DS model returns 5 rows with
synonyms populated. A model with a malformed expression fails `ossie_load` with a
message naming the offending metric. CI builds and tests on Linux and macOS.

*Note: DuckDB vendors yyjson but no YAML parser. JSON-only is not a shortcut — dbt's
Ossie implementation also accepts only `.json`.*

### Phase 3a — Star-join compiler + MCP

Real per-request planning, easy case only.

- Stages 1, 2, 4, 5 of the compiler
- Single fact table, star topology
- Cardinality flags computed at load time; **refuse** any query where a single aggregate's
  arguments span datasets, or where more than one fact table is required
- A metric *may* reference a second dataset as a scalar — `item.i_category` inside a `CASE`,
  say — provided that dataset is reached only across many-to-one edges. That is an ordinary
  join with no fan-out, and it needs none of phase 3c's machinery. Do not lump it in with
  genuinely multi-grain metrics and defer it
- `ossie_compile` and `ossie_query`
- MCP publication wired up at the end of this phase

**Exit:** `total_sales` by `item.i_brand` filtered on `date_dim.d_year` returns correct
rows against loaded TPC-DS data. `store_productivity` errors with a message naming the
metric and the fanned-out dataset. Claude Desktop, pointed at
`duckdb -init server.sql`, answers "top brands by revenue in 2001" correctly.

*The MCP step is a day of work and belongs here, not later. It is where you learn
whether JSON-string parameters are the right shape, whether the agent reliably picks
names from synonyms, and what the error messages need to say. Discovering the tool
surface is wrong after phase 3c is expensive.*

### Phase 3b — Join correctness

Each of these is a separate decision with a separate failure mode:

- **Ambiguous paths.** More than one route between two datasets. Refuse with both paths
  named. Add an optional explicit `path` argument only if a real model demands it.
- **Snowflaked models.** Connecting two requested datasets pulls in a third nobody
  asked for. Legal, but it changes row counts — make sure the grain check sees it.
- **Join type.** Ossie declares the edge but not `INNER` vs `LEFT`. If the FK column is
  nullable, `INNER` silently drops fact rows. Pick a default (`LEFT` from fact to
  dimension is the safer choice), document it, allow override.
- **Role-playing dimensions.** `date_dim` joined once as sold-date and once as
  returned-date. Requires per-edge aliasing and qualified dimension references that
  disambiguate which role is meant.

**Exit:** golden-file SQL for a dozen real TPC-DS queries expressed semantically;
differential tests confirm result sets match the hand-written originals row for row.

### Phase 3c — Multi-grain and multi-fact

Turn 3b's refusals into answers.

- Determine each *aggregate's* grain from the datasets its own arguments reference. Grain is
  a property of each aggregate node, not of the metric as a whole: a metric may reference
  three datasets and still contain a single aggregate at a single grain
- When grains differ, emit one CTE per grain aggregating at that grain, then join the
  CTEs on shared dimensions
- Multiple fact tables fall out of the same machinery — they are metrics that cannot
  share a grain

Do **not** implement symmetric aggregates (the `SUM(DISTINCT hash(pk) * 2^n + value)`
trick). It is clever and one-pass, but brittle with floats and hash collisions. The CTE
approach is correct and debuggable.

**Exit:** `store_productivity` returns a correct number when grouped by a dimension every
grain can reach, such as `store.s_state`. Grouping it by `item.i_brand` stays refused, and
correctly so: the per-store CTE reaches `item` only back through `store_sales`, so there is
no shared dimension to join the CTEs on. Multi-grain support means requests with a shared
groupable dimension work — not that every multi-grain request does. Record that residual
refusal in `docs/limitations.md`. A query mixing `store_sales` and `web_sales` metrics
grouped by a shared dimension works.

### Phase 5 — Hardening and release

- YAML support (vendor rapidyaml)
- Swappable dialect emitter so `ossie_compile` can target Snowflake / BigQuery /
  Postgres. Keep the AST dialect-neutral and the printer pluggable so this stays an
  addition rather than a rewrite.
- Ossie version handling. The example model is `version: "0.2.0.dev0"` while dbt
  accepts only 0.1.0/0.1.1. Pin a supported range, error clearly outside it, expect
  churn.
- Submission to the DuckDB community extensions repo

---

## 7. Testing

**Golden-file SQL.** `ossie_compile` output for a fixed set of semantic queries,
checked into the repo. No database needed. This is the fastest signal and should run on
every commit.

**Differential.** Run the generated SQL and the hand-written TPC-DS equivalent against
the same data; diff result sets. This is the only test that catches grain bugs. Build
the harness in phase 3a even though it only has a few cases then.

**Negative.** Every refusal path needs a test asserting both that it errors *and* what
the message says. The error messages are part of the interface — an agent reads them
and retries.

**Fixtures.** Generate TPC-DS at SF=1 via DuckDB's `tpcds` extension. Keep a second,
deliberately nasty model in the fixtures: multiple fact tables, an ambiguous join path,
a role-playing dimension, a nullable FK. Most phases are defined by which of its
queries they can answer.

---

## 8. Known pitfalls

**DuckDB internal APIs are not stable.** `ParsedExpression`, `SelectNode`, `JoinRef`,
and `bind_replace` signatures shift between releases. Pin the version in the template
and expect churn on upgrade. This is the cost every community extension pays; the
alternative (bringing your own AST layer) means reimplementing expression printing and
precedence.

**`unique_keys` vs `primary_key`.** Checking only one will misclassify edge cardinality
and silently disable the fan-out guard. Check both.

**Computed fields must be inlined, not referenced.** `customer_full_name` is
`c_first_name || ' ' || c_last_name` — emitting `customer.customer_full_name` produces a
column-not-found error against the physical table.

**Dialect selection.** Fields carry `dialects: [{dialect: ANSI_SQL, expression: ...}]`.
Prefer a `DUCKDB` dialect if present, fall back to ANSI, and validate the fragment
parses before using it.

**`ToString()` emits DuckDB dialect.** If `ossie_compile` is meant to target other
engines, that needs a separate emitter (phase 5), not a post-processing regex.

**Data locality.** The extension executes SQL, so DuckDB must be able to reach the
tables. Natural for Parquet, Iceberg, local files, or attached Postgres. If the data
lives in Snowflake, `ossie_compile` is the useful surface and something else executes.

---

## 9. Suggested repo layout

```
duckdb_ossie/
  src/
    ossie_extension.cpp          registration
    model/
      parser.cpp                 JSON → Model
      model.hpp                  Model, Dataset, Field, Metric, Relationship
      validate.cpp               load-time checks
      graph.cpp                  relationship graph, cardinality flags
    compile/
      resolve.cpp                stage 1
      join_planner.cpp           stage 2
      grain.cpp                  stage 3
      rewrite.cpp                stage 4 — ParsedExpression rewriting
      emit.cpp                   stage 5 — SelectNode assembly
      dialect/                   phase 5
    functions/
      load.cpp
      introspect.cpp
      query.cpp                  ossie_query (bind_replace)
      compile.cpp                ossie_compile
  test/
    sql/                         sqllogictest
    golden/                      expected SQL
    fixtures/
      tpcds_semantic_model.json
      nasty_model.json
  docs/
    limitations.md               what refuses to answer, and why
```

`docs/limitations.md` is not filler. Phases 3a and 3b ship with real refusals, and
documenting them precisely is what makes the extension trustworthy rather than
half-finished — it is also the most interesting thing to bring to an Ossie spec
discussion, since it identifies exactly where the format underdetermines execution.

---

## 10. Scope guidance

Phases 0 and 3a are the credible artifact — enough to link in a PR or a working-group
discussion. Phase 3b is where it becomes broadly useful. Phase 3c is where it becomes
complete.

If time runs short, a narrow extension that refuses clearly beats a general one that
guesses. Resist letting 3b's cases leak into 3a; the ambiguity work is open-ended and
will absorb whatever time it is given.