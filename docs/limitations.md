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

**One semantic model per file.** The format permits an array of them, but metrics are addressed by
bare name with no way to say which model is meant. Merging them would let two identically named
datasets bind to different physical tables and silently return a number from the wrong one.

**Only ANSI_SQL expressions.** The dialect enum has no DuckDB member, so `ANSI_SQL` is the only
executable dialect. A field or metric carrying only a `SNOWFLAKE` or `MDX` expression fails at
load rather than being guessed at.

**Table sources only.** `source` may name a query rather than a table under the spec. Such a
source can neither be prefix-rebound nor bound as a table reference, so it is refused at load.

**Unqualified columns in metrics.** A metric must write `store_sales.ss_ext_sales_price` rather
than `ss_ext_sales_price`. A bare column cannot say which dataset it belongs to, and a metric's
dataset is its grain.
