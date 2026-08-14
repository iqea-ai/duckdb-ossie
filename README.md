# duckdb_ossie

A DuckDB extension that reads [Apache Ossie](https://github.com/apache/ossie) (incubating)
semantic model files and answers semantic queries against the tables they describe.

Ossie is a vendor-neutral file format for semantic models: `datasets` bound to physical
tables, `fields`, declared `relationships`, and `metrics` written as aggregate expressions.
It describes data in place and never transforms it. What the spec deliberately leaves open
is the *query* those definitions go into — the `JOIN`s, the `GROUP BY`, and the grain — because
those depend on the question being asked, and the file does not know the question.

Today the shipped Ossie implementations are all *converters*, translating definitions into
some vendor's semantic layer so that vendor's engine can run them. This extension is the
missing piece: an embedded, single-binary engine that compiles a semantic request into SQL
and executes it, with no infrastructure.

```sql
CALL ossie_load('tpcds_semantic_model.json', rebind => MAP{'tpcds.public': 'tpcds.main'});

SELECT * FROM ossie_metrics();                                   -- discover vocabulary
SELECT * FROM ossie_query(['total_sales'], ['item.i_brand'],     -- execute
                          ['date_dim.d_year = 2001']);
SELECT ossie_compile(['total_sales'], ['item.i_brand'], []);     -- or just emit the SQL
```

## Status

Early. The extension currently builds and loads; the model parser, introspection functions,
and compiler are being implemented in phases. See [ossie-duckdb-plan.md](ossie-duckdb-plan.md)
for the full spec and phase breakdown.

## Design invariants

These hold everywhere, and outrank feature coverage:

1. **Never return a wrong number.** An agent querying this extension cannot check the answer,
   so a plausible wrong number is worse than a failure. Refusing with a clear error is always
   correct behavior.
2. **Never build SQL by string concatenation.** Fragments are parsed into `ParsedExpression`
   trees at load time, rewritten as trees, and emitted from the tree.
3. **Fail at load time, not query time.** Malformed expressions, unresolvable `source:` names,
   and dangling relationship endpoints are `ossie_load` errors.
4. **`ossie_compile()` is a first-class output**, not a debug aid — it is the useful surface
   wherever DuckDB is not the executor.
5. **The model is the authorization boundary.** A name absent from the model does not resolve,
   and there is no escape hatch to reference an unmodeled column.

Because refusals are part of the interface, what the extension declines to answer — and why —
is documented in `docs/limitations.md` rather than left implicit.

## Building

No vcpkg or third-party dependencies are required; JSON parsing uses the yyjson that DuckDB
already vendors.

```sh
git submodule update --init --recursive
make
```

Submodules are pinned by recorded commit (DuckDB `v1.5.4`, extension-ci-tools `v1.5-variegata`).
DuckDB's parser and planner internals are not a stable API, so the pin is deliberate and
upgrades are expected to need work. The first build compiles DuckDB from source and takes a
while; later builds are incremental.

Binaries produced:

```sh
./build/release/duckdb                                     # shell with the extension preloaded
./build/release/test/unittest                              # test runner
./build/release/extension/ossie/ossie.duckdb_extension     # loadable binary
```

## Testing

```sh
make test
```

The suite has three layers, per the plan: golden-file `ossie_compile` output (fast, needs no
database), differential tests that run generated SQL against hand-written TPC-DS equivalents
and diff the result sets, and negative tests asserting both *that* a query is refused and
*what the message says*.

## License

MIT. See [LICENSE](LICENSE).