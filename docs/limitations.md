# What duckdb_ossie refuses, and why

A refusal is a feature here. An agent querying this extension cannot check the answer it gets
back, so a plausible wrong number is worse than an error. Everything below is a case where the
model, the request, or the format underdetermines the query — and the extension declines rather
than picking.

## Grain

**Metrics that aggregate at more than one grain.** `store_productivity` is
`SUM(store_sales.ss_ext_sales_price) / NULLIF(SUM(store.s_number_employees), 0)`. Joining `store`
to `store_sales` replicates each store's employee count once per sale line, so a naive `SUM` over
it is inflated by roughly the number of sales per store. Answering this correctly requires
computing each aggregate at its own grain and joining the results, which is not implemented.
`customer_lifetime_value` is refused for the same reason.

Note that a metric referencing a second dataset is *not* by itself a problem. Where the second
dataset is functionally determined by the grain — `item.i_category` inside an aggregate over
`store_sales` — its values are replicated onto fact rows without changing their count, and the
query compiles.

**Aggregates with no column reference.** A metric defined as `COUNT(*)` names no dataset, and
Ossie has no field binding a metric to one. Asking for it alone leaves no candidate for the `FROM`
clause; against the TPC-DS model the answer could be 2.9M (sales), 100K (customers) or 12
(stores), with nothing to choose between them. Requesting it *alongside* a metric that does have a
grain works, since it then inherits that grain. Model authors can avoid the case entirely by
writing `COUNT(store_sales.ss_item_sk)`.

**Window-function metrics.** A metric such as `SUM(SUM(store_sales.ss_ext_sales_price)) OVER (ORDER
BY date_dim.d_date ...)` fixes its partition and order in the model, but the rows it runs over are
the groups of whatever dimensions the request names. Asked for by date and item category over
TPC-DS data, that running total ran across categories, and 10,047 of 10,049 rows differed from a
per-category running total; ties on date also leave the order, and so the value, arbitrary. Any
metric containing a window function (`OVER`) is refused when queried, naming the metric and the
function. The model still loads. Upstream's own `examples/tpcds_semantic_model.yaml` has three such
metrics.

**Fan-out joins.** If reaching a dataset repeats rows of the grain — a many-to-many relationship,
or traversing a many-to-one edge backwards — every aggregate in the query would be inflated. Such
a join is refused whether it was pulled in by a metric, a dimension, or a filter.

## Joins

**Ambiguous paths.** When two distinct routes connect the grain to a required dataset, they can
produce different numbers, so the request is refused with the endpoints named rather than resolved
to whichever route was found first.

**Join type is not declared.** Ossie describes a relationship's endpoints and columns but not
whether it is inner or outer. All joins are emitted as `INNER`, which means a fact row whose
foreign key is `NULL` is dropped once the corresponding dimension is joined. A metric can
therefore return a smaller total once a dimension or filter is added. There is currently no
override.

## Filters

Filter predicates arrive per request rather than from the model, so their contents are
allowlisted. Operators are always permitted, including arithmetic, `||` and `LIKE`. Named function
calls are refused unless the model was loaded with `allow_filter_functions => true` — note that
`SIMILAR TO` desugars to a `regexp_full_match` call and so falls under that flag, while `LIKE`
does not. Subqueries are refused unconditionally and no option enables them, since they could read
tables the model never declared.

## Model conformance

**The pre-0.2 `semantic_model` array.** Ossie 0.2.0.dev0 puts exactly one model at the root of each
document and removed the `semantic_model` array rather than deprecating it. A document that still
wraps its model is refused at load with the migration spelled out: move the model's properties to
the top level, delete the wrapper, and split a multi-model file into one file per model.

**Two executable dialects.** An expression runs if it has an `OSSIE_SQL_2026` or an `ANSI_SQL`
variant; when it has both, `OSSIE_SQL_2026` wins, since the spec defines what it means. A field or
metric carrying only a `SNOWFLAKE`, `DATABRICKS` or `MDX` expression fails at load rather than
being guessed at.

**OSSIE_SQL_2026 constructs with no defined result.** The expression language
(`core-spec/expression_language.md`) is lowered to DuckDB at load. Where it leaves a result
undefined and engines disagree, the expression is refused at load, naming the field or metric:

- `REGEXP_LIKE`: Snowflake requires the pattern to match the whole string, Databricks and
  BigQuery any part of it, and the language does not say. Use `LIKE`, or give the expression an
  `ANSI_SQL` variant that says which is meant (for example `regexp_full_match`).
- `TO_DATE` or `TO_TIMESTAMP` with a format argument: format models differ between engines, and the
  form is `EXPERIMENTAL` in the language. The one-argument ISO-8601 form is supported.
- A date part outside year, quarter, month, week, day, hour, minute and second, or a function called
  with a different number of arguments than the language defines.

**OSSIE_SQL_2026 choices where DuckDB differs.** DuckDB's `concat`, `greatest` and `least` skip
NULL arguments. The language's own `||` and standard SQL return NULL when any argument is NULL, so
`CONCAT`, `GREATEST` and `LEAST` do too here. `DATE_TRUNC` and `DATEADD` on a `DATE` return a
`TIMESTAMP` at midnight in DuckDB rather than a `DATE`; the value is the same, the type is not. A
function outside the language that DuckDB does not know is not refused at load: it reaches DuckDB's
binder at query time, as it would in an `ANSI_SQL` expression.

Request filters are read in the same language, whatever dialect the model's own expressions use: a filter
is lowered exactly as an `OSSIE_SQL_2026` field is, after `allow_filter_functions` has judged the filter
as written. So `NVL` and `DATEADD(day, 30, d)` work in a filter, and `CONCAT(a, b) IS NULL` is true when
either is NULL, as it would be in a field.

**Quoted identifiers compare case-insensitively.** The language makes a quoted identifier exact: `"id"`
should not match a column created as `id` (normalised to `ID`). DuckDB matches quoted names
case-insensitively, and so does this extension. Only a model that relies on two names differing solely in
case, one of them quoted, can tell the difference.

**Table sources only.** `source` may name a query rather than a table under the spec. Such a
source can neither be prefix-rebound nor bound as a table reference, so it is refused at load.

**Unqualified columns in metrics.** A metric must write `store_sales.ss_ext_sales_price` rather
than `ss_ext_sales_price`. A bare column cannot say which dataset it belongs to, and a metric's
dataset is its grain. Note this is a requirement about *qualification*, not about declaration: the
column itself need not appear in the dataset's `fields`.

**Columns that are not declared fields are passed through, not refused.** The format requires
neither that a relationship's columns be declared fields nor that a field or metric expression
reference only declared fields, and real third-party models rely on both. Such a name is treated as
a physical column and resolved by DuckDB's binder. The tradeoff is deliberate and has a cost: a
genuine typo in a model expression is no longer caught by `ossie_load`, and instead surfaces as a
binder error when a query touches it. That message names the column and the table, so it is a worse
message arriving later — not a wrong answer.

## Known DuckDB defects

**Intervals of 30 days and one month are treated as the same expression.** DuckDB's optimizer
folds `INTERVAL 30 DAY` and `INTERVAL 1 MONTH` to constants, compares them with a month normalised
to 30 days, and merges the two expressions as duplicates, so a query that asks for both gets
whichever comes first, twice. Adding a month to 2024-01-31 should give 2024-02-29, and does when
asked alone. This is DuckDB 1.5.5 and 1.5.6 with no extension loaded, and it affects `ANSI_SQL`
models (`d + INTERVAL 30 DAY` beside `d + INTERVAL 1 MONTH`) as much as `OSSIE_SQL_2026` ones
(`DATEADD(day, 30, d)` beside `DATEADD(month, 1, d)`). Disabling the `common_subexpressions`
optimizer gives the right answer. `test/sql/ossie_sql_2026.test` pins the current behaviour, so an
upgrade that fixes it fails there and this entry can go. Minimal repro:

```sql
CREATE TABLE t AS SELECT DATE '2024-01-31' AS d;
SELECT d + INTERVAL 30 DAY, d + INTERVAL 1 MONTH FROM t;   -- 2024-03-01 | 2024-03-01
```
