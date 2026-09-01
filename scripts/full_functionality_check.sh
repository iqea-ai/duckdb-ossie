#!/usr/bin/env bash
# End-to-end functionality check for the ossie extension, driven the way an end user drives it:
# a real duckdb CLI, a real database file, real semantic queries, and asserted NUMBERS.
#
# Deliberately not the sqllogictest suite. `make test` runs inside a build tree with the extension
# statically linked. This drives the CLI and, when the ABI matches, the loadable
# .duckdb_extension artifact -- which is what a user actually installs.
#
# Usage:  scripts/full_functionality_check.sh
# Exit:   0 = all assertions passed (known defects are reported loudly but do not gate)
#         1 = at least one assertion failed
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIX="$REPO/test/fixtures"
EXT="$REPO/build/release/extension/ossie/ossie.duckdb_extension"
BUILT="$REPO/build/release/duckdb"
TMP="$(mktemp -d)"; DB="$TMP/tpcds.db"      # file stem becomes the catalog name -> tpcds.main.*
trap 'rm -rf "$TMP"' EXIT

PASS=0; FAIL=0; XFAIL=0; SKIP=0
g(){  printf '\n\033[1m== %s\033[0m\n' "$1"; }
ok(){ PASS=$((PASS+1)); printf '  \033[32mPASS \033[0m %s\n' "$1"; }
bad(){ FAIL=$((FAIL+1)); printf '  \033[31mFAIL \033[0m %s\n         want: %s\n         got:  %s\n' "$1" "$2" "$3"; }
xf(){ XFAIL=$((XFAIL+1)); printf '  \033[33mXFAIL\033[0m %s\n         %s\n' "$1" "$2"; }
sk(){ SKIP=$((SKIP+1)); printf '  \033[36mSKIP \033[0m %s (%s)\n' "$1" "$2"; }

# --------------------------------------------------------------- choose the CLI
STOCK="$(command -v duckdb || true)"; WHY=""
if [ -n "$STOCK" ] && [ -f "$EXT" ] && \
   "$STOCK" -unsigned -noheader -list -c "LOAD '$EXT'; SELECT 1;" >/dev/null 2>&1; then
  DUCKDB="$STOCK"; EXTRA="-unsigned"
  PREAMBLE="LOAD '$EXT'; INSTALL tpcds; LOAD tpcds;"
  MODE="stock CLI $("$STOCK" --version | awk '{print $1}') + loadable artifact   [FAITHFUL]"
else
  DUCKDB="$BUILT"; EXTRA=""; PREAMBLE=""
  MODE="build-tree shell $("$BUILT" --version 2>/dev/null | awk '{print $1}'), statically linked   [FALLBACK]"
  [ -n "$STOCK" ] && WHY="$("$STOCK" -unsigned -noheader -list -c "LOAD '$EXT'; SELECT 1;" 2>&1 | head -1)" \
                  || WHY="no duckdb on PATH"
fi

printf '\033[1mossie end-to-end functionality check\033[0m\nmode: %s\n' "$MODE"
[ -n "$WHY" ] && printf '\033[33mloadable path unavailable:\033[0m %s\n' "$WHY"

q(){ "$DUCKDB" $EXTRA -noheader -list "$DB" -c "$PREAMBLE $1" 2>&1; }
eq(){  local n="$1" w="$2" got; got="$(q "$3" | tail -1)"; [ "$got" = "$w" ] && ok "$n" || bad "$n" "$w" "$got"; }
has(){ local n="$1" w="$2" got; got="$(q "$3" | tr '\n' ' ')"; case "$got" in *"$w"*) ok "$n";; *) bad "$n" "*$w*" "${got:0:160}";; esac; }

LOAD_T="CALL ossie_load('$FIX/tpcds_semantic_model.json', rebind => MAP{'tpcds.public':'tpcds.main'});"

# --------------------------------------------------------------- J0
g "J0  installable, and the surface registers"
eq "extension reports loaded"      "true" "SELECT loaded FROM duckdb_extensions() WHERE extension_name='ossie';"
eq "7 ossie functions registered"  "7"    "SELECT count(DISTINCT function_name) FROM duckdb_functions() WHERE starts_with(function_name, 'ossie_');"

# --------------------------------------------------------------- J1
g "J1  using it wrong fails usefully"
has "ossie_query names the fix"   "ossie_load" "SELECT * FROM ossie_query(['total_sales']);"
has "ossie_compile names the fix" "ossie_load" "SELECT ossie_compile(['total_sales'],[],[]);"

# --------------------------------------------------------------- J2
g "J2  load a model against real tables"
q "CALL dsdgen(sf = 0.01);" >/dev/null 2>&1 || true
eq "TPC-DS data present" "true" "SELECT count(*) > 0 FROM tpcds.main.store_sales;"
eq "model loads and every source resolves" "tpcds_retail_model|0.2.0.dev0|5|31|4|5" \
   "CALL ossie_load('$FIX/tpcds_semantic_model.json', rebind => MAP{'tpcds.public':'tpcds.main'}, validate_sources => true);"
has "a wrong rebind reports all unresolved sources at once" "dataset sources do not exist" \
   "CALL ossie_load('$FIX/tpcds_semantic_model.json', rebind => MAP{'tpcds.public':'nope.nope'}, validate_sources => true);"

# --------------------------------------------------------------- J3
g "J3  an agent can discover the vocabulary"
eq "5 metrics"                "5" "$LOAD_T SELECT count(*) FROM ossie_metrics();"
eq "31 fields"               "31" "$LOAD_T SELECT count(*) FROM ossie_fields();"
eq "all 5 datasets resolve"   "5" "$LOAD_T SELECT count(*) FROM ossie_datasets() WHERE resolved;"
eq "synonyms populated"    "true" "$LOAD_T SELECT count(*) > 0 FROM ossie_metrics() WHERE len(synonyms) > 0;"
eq "cardinality derived: 4 many_to_one" "4" "$LOAD_T SELECT count(*) FROM ossie_relationships() WHERE cardinality='many_to_one';"
eq "time dimension flagged" "true" "$LOAD_T SELECT count(*) > 0 FROM ossie_fields() WHERE is_time;"
eq "computed field flagged" "true" "$LOAD_T SELECT is_computed FROM ossie_fields() WHERE name='customer_full_name';"

# --------------------------------------------------------------- J4
g "J4  the answer is CORRECT, not merely non-erroring"
eq "ungrouped metric == plain SUM" "0.00" \
  "$LOAD_T SELECT (SELECT total_sales FROM ossie_query(['total_sales']))
                - (SELECT sum(ss_ext_sales_price) FROM tpcds.main.store_sales);"
eq "grouped by brand == hand-written join (symmetric EXCEPT)" "0" \
  "$LOAD_T SELECT count(*) FROM (
     (SELECT * FROM ossie_query(['total_sales'],['item.i_brand'])
      EXCEPT SELECT i_brand, sum(ss_ext_sales_price) FROM tpcds.main.store_sales
        JOIN tpcds.main.item ON ss_item_sk=i_item_sk GROUP BY i_brand)
     UNION ALL
     (SELECT i_brand, sum(ss_ext_sales_price) FROM tpcds.main.store_sales
        JOIN tpcds.main.item ON ss_item_sk=i_item_sk GROUP BY i_brand
      EXCEPT SELECT * FROM ossie_query(['total_sales'],['item.i_brand'])));"
eq "a filter pulls in a 2nd join and still matches" "0" \
  "$LOAD_T SELECT count(*) FROM (
     (SELECT * FROM ossie_query(['total_sales'],['item.i_brand'],['date_dim.d_year = 2001'])
      EXCEPT SELECT i_brand, sum(ss_ext_sales_price) FROM tpcds.main.store_sales
        JOIN tpcds.main.item ON ss_item_sk=i_item_sk
        JOIN tpcds.main.date_dim ON ss_sold_date_sk=d_date_sk WHERE d_year=2001 GROUP BY i_brand)
     UNION ALL
     (SELECT i_brand, sum(ss_ext_sales_price) FROM tpcds.main.store_sales
        JOIN tpcds.main.item ON ss_item_sk=i_item_sk
        JOIN tpcds.main.date_dim ON ss_sold_date_sk=d_date_sk WHERE d_year=2001 GROUP BY i_brand
      EXCEPT SELECT * FROM ossie_query(['total_sales'],['item.i_brand'],['date_dim.d_year = 2001'])));"
eq "computed dimension (no such column on the table) works" "true" \
  "$LOAD_T SELECT count(*) > 0 FROM ossie_query(['total_sales'],['customer.customer_full_name']);"
eq "documented INNER shortfall == exactly the NULL-FK sales" "0.00" \
  "$LOAD_T SELECT (SELECT total_sales FROM ossie_query(['total_sales']))
                - (SELECT sum(total_sales) FROM ossie_query(['total_sales'],['store.s_state']))
                - (SELECT sum(ss_ext_sales_price) FROM tpcds.main.store_sales WHERE ss_store_sk IS NULL);"

# --------------------------------------------------------------- J5  inspect == run
g "J5  ossie_compile is first-class: copy the SQL out, run it, same rows"
CSQL="$(q "$LOAD_T SELECT ossie_compile(['total_sales'],['item.i_brand'],[]);" | tail -1)"
case "$CSQL" in
  SELECT*)
    A="$(q "$LOAD_T $CSQL" | sort | shasum | awk '{print $1}')"
    B="$(q "$LOAD_T SELECT * FROM ossie_query(['total_sales'],['item.i_brand']);" | sort | shasum | awk '{print $1}')"
    [ "$A" = "$B" ] && ok "hand-run compiled SQL == ossie_query rows" \
                    || bad "hand-run compiled SQL == ossie_query rows" "$B" "$A";;
  *) bad "ossie_compile returned SQL" "SELECT..." "${CSQL:0:120}";;
esac
eq "compile works with no tables at all (pre-rebind)" "true" \
   "CALL ossie_load('$FIX/tpcds_semantic_model.json'); SELECT ossie_compile(['total_sales'],[],[]) LIKE 'SELECT%';"

# --------------------------------------------------------------- J6  refusals
g "J6  refusals -- the reason to trust this with an agent"
has "multi-grain refused, both grains named" "different grains (store, store_sales)" \
   "CALL ossie_load('$FIX/grain_model.json'); SELECT ossie_compile(['store_productivity'],[],[]);"
has "2nd multi-grain metric refused" "different grains (customer, store_sales)" \
   "CALL ossie_load('$FIX/grain_model.json'); SELECT ossie_compile(['customer_lifetime_value'],[],[]);"
has "mixing a clean metric with a multi-grain one refused" "different grains" \
   "CALL ossie_load('$FIX/grain_model.json'); SELECT ossie_compile(['total_sales','store_productivity'],[],[]);"
has "COUNT(*) metric alone refused (no grain to group by)" "no column reference" \
   "CALL ossie_load('$FIX/grain_model.json'); SELECT ossie_compile(['transaction_count'],[],[]);"
has "scalar reference to a 2nd dataset COMPILES (not a fan-out)" "INNER JOIN" \
   "CALL ossie_load('$FIX/grain_model.json'); SELECT ossie_compile(['books_sales'],[],[]);"
has "fan-out join refused, relationship named" "would inflate every aggregate" \
   "CALL ossie_load('$FIX/many_to_many_model.json'); SELECT ossie_compile(['total_sales'],['store.s_state'],[]);"
has "ambiguous join path refused, endpoints named" "more than one join path" \
   "CALL ossie_load('$FIX/ambiguous_path_model.json'); SELECT ossie_compile(['total_sales'],['item.i_brand'],[]);"
has "unknown metric suggests a candidate" "Did you mean" \
   "$LOAD_T SELECT ossie_compile(['total_sales_typo'],[],[]);"
has "unknown dimension suggests a candidate" "Did you mean" \
   "$LOAD_T SELECT ossie_compile(['total_sales'],['item.i_brandd'],[]);"
has "subquery in a filter refused unconditionally" "subquery" \
   "$LOAD_T SELECT ossie_compile(['total_sales'],[],['item.i_brand IN (SELECT 1)']);"
has "window function in a filter refused" "not allowed" \
   "$LOAD_T SELECT ossie_compile(['total_sales'],[],['row_number() OVER () > 0']);"

g "J6b  malformed models rejected at LOAD, not at query time"
for f in dangling_relationship malformed_expression unqualified_metric; do
  has "invalid/$f rejected at load" "ossie_load:" "CALL ossie_load('$FIX/invalid/$f.json');"
done

# --------------------------------------------------------------- J7  policy
g "J7  filter policy is fixed at load, not widenable per request"
has "function call refused when the flag is off" "allow_filter_functions => true" \
   "$LOAD_T SELECT ossie_compile(['total_sales'],[],['upper(item.i_brand) IS NOT NULL']);"
eq "same filter permitted when the flag is on" "true" \
   "CALL ossie_load('$FIX/tpcds_semantic_model.json', rebind => MAP{'tpcds.public':'tpcds.main'}, allow_filter_functions => true);
    SELECT ossie_compile(['total_sales'],[],['upper(item.i_brand) IS NOT NULL']) LIKE '%upper%';"
has "LIKE is an operator, so it passes with the flag off" "SELECT" \
   "$LOAD_T SELECT ossie_compile(['total_sales'],[],['item.i_brand LIKE item.i_brand']);"

# --------------------------------------------------------------- known defects
g "KNOWN DEFECTS  (reported, do not gate exit code)"
NEST="$TMP/nested.json"
python3 - "$NEST" <<'PY'
import json,sys
def f(n,e=None): return {"name":n,"datatype":"String",
  "expression":{"dialects":[{"dialect":"ANSI_SQL","expression":e or n}]}}
json.dump({"version":"0.2.0.dev0","semantic_model":[{"name":"nested","datasets":[
 {"name":"nf","source":"tpcds.main.nf","fields":[f("k1"),f("amt")]},
 {"name":"nd","source":"tpcds.main.nd","primary_key":["k1"],
  "fields":[f("k1"),f("label"),f("mid","label || 'M'"),f("outer_f","mid || 'O'")]}],
 "relationships":[{"name":"r","from":"nf","to":"nd","from_columns":["k1"],"to_columns":["k1"]}],
 "metrics":[{"name":"total","expression":{"dialects":[{"dialect":"ANSI_SQL","expression":"SUM(nf.amt)"}]}}]}]},
 open(sys.argv[1],"w"))
PY
NOUT="$(q "CREATE OR REPLACE TABLE tpcds.main.nf(k1 INT, amt DECIMAL(10,2));
           CREATE OR REPLACE TABLE tpcds.main.nd(k1 INT, label VARCHAR, mid VARCHAR);
           INSERT INTO tpcds.main.nf VALUES (1,10.00);
           INSERT INTO tpcds.main.nd VALUES (1,'REAL','DECOY');
           CALL ossie_load('$NEST');
           SELECT * FROM ossie_query(['total'],['nd.outer_f']);" | tail -1)"
[ "$NOUT" = "REALMO|10.00" ] \
  && ok "nested computed field resolves transitively" \
  || xf "nested computed field shadowed by a physical column" \
        "model defines outer_f=(label||'M')||'O' -> want REALMO|10.00, got '$NOUT'. InlineFields does not recurse."

# --------------------------------------------------------------- optional MCP
g "MCP publication (optional)"
MCPOUT="$(q "INSTALL duckdb_mcp FROM community; SELECT 'mcp ok';" 2>&1 | tail -1)"
if [ "$MCPOUT" != "mcp ok" ]; then
  sk "MCP server surface" "duckdb_mcp unavailable: ${MCPOUT:0:70}"
else
  # An extension's PRAGMAs cannot be bound in the same -c batch as the LOAD that registers them:
  # DuckDB binds the whole batch before executing it. examples/server.sql avoids this by using
  # -init, so the check has to as well -- otherwise it tests the harness, not the extension.
  MI="$TMP/mcp_init.sql"
  { [ -n "$PREAMBLE" ] && printf '%s\n' "$PREAMBLE"; printf "LOAD duckdb_mcp;\n"; } > "$MI"
  MRES="$("$DUCKDB" $EXTRA -noheader -list -init "$MI" "$DB" -c \
    "$LOAD_T PRAGMA mcp_publish_query('metrics','SELECT name FROM ossie_metrics()');
     SELECT * FROM mcp_resources();" 2>&1 | tr '\n' ' ')"
  case "$MRES" in
    *metrics*) ok "model published as an MCP resource (server.sql path)";;
    *) bad "model published as an MCP resource (server.sql path)" "*metrics*" "${MRES:0:150}";;
  esac
fi

# --------------------------------------------------------------- summary
printf '\n\033[1m--------------------------------------------------\033[0m\n'
printf 'PASS %d   FAIL %d   XFAIL %d   SKIP %d\n%s\n' "$PASS" "$FAIL" "$XFAIL" "$SKIP" "$MODE"
[ "$FAIL" -eq 0 ] && printf '\033[32mFUNCTIONALITY CHECK GREEN\033[0m\n' || printf '\033[31mFUNCTIONALITY CHECK RED\033[0m\n'
[ "$FAIL" -eq 0 ]
