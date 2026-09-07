"""End-to-end check of the MCP surface.

Speaks JSON-RPC over stdio to `duckdb -init examples/server.sql`, exactly as Claude Desktop
does, and asserts an agent can complete a round trip: handshake, discover the tool, discover the
vocabulary, get real rows back, and receive a refusal as a readable error.

Nothing else covers this path. `make test` never starts a server, and the MCP leg of
scripts/full_functionality_check.sh only asserts that a resource can be *published* -- not that a
client can connect and query. Writing this found two defects that had shipped in v0.1.0:
examples/server.sql never loaded the ossie extension, so the documented MCP path failed immediately
for anyone installing from the registry; and duckdb_mcp publishes generic query/export tools
alongside ours, which falsified a security claim in the README.

Usage:  python3 scripts/mcp_check.py        (run from the repository root)
Exit:   0 all checks passed, 1 otherwise
"""
import json, os, shutil, subprocess, sys, tempfile, time

if shutil.which("duckdb") is None:
    print("duckdb is not on PATH. Install the CLI, or run this from an environment that has it.")
    sys.exit(2)

try:
    proc = subprocess.Popen(
        ["duckdb", "-init", "examples/server.sql"],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, bufsize=1)
except OSError as exc:
    print(f"could not start the MCP server: {exc}")
    sys.exit(2)

def send(obj):
    proc.stdin.write(json.dumps(obj) + "\n"); proc.stdin.flush()

def read(timeout=60):
    start = time.time()
    while time.time() - start < timeout:
        line = proc.stdout.readline()
        if not line:
            time.sleep(0.05); continue
        line = line.strip()
        if not line.startswith("{"):
            continue
        try: return json.loads(line)
        except json.JSONDecodeError: continue
    return None

ok = True
def check(label, cond, detail=""):
    global ok
    print(f"  {'PASS' if cond else 'FAIL'}  {label}" + (f"   {detail}" if detail else ""))
    ok = ok and cond

send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{
    "protocolVersion":"2024-11-05","capabilities":{},
    "clientInfo":{"name":"ossie-probe","version":"1"}}})
r = read()
check("initialize handshake", bool(r and "result" in r),
      (r or {}).get("result",{}).get("serverInfo",{}).get("name",""))

send({"jsonrpc":"2.0","method":"notifications/initialized"})

send({"jsonrpc":"2.0","id":2,"method":"tools/list"})
r = read()
tools = [t["name"] for t in (r or {}).get("result",{}).get("tools",[])]
# Exclusivity, not membership. "semantic_query in tools" was the original assertion and it passed
# throughout v0.1.0 while query/export were also exposed -- it cannot fail when the hole is open.
check("tools/list exposes semantic_query AND NOTHING ELSE", tools == ["semantic_query"], str(tools))

# Absence from the listing is not the same as refusal on call. Assert the gate, not the menu.
# export is checked as well as query because it takes an arbitrary SQL argument: disabling query
# alone leaves a second route to the same data (teaguesterling/duckdb_mcp#75).
for builtin in ("query", "export"):
    send({"jsonrpc":"2.0","id":20,"method":"tools/call",
          "params":{"name":builtin,"arguments":{"sql":"SELECT 1"}}})
    r = read()
    err = json.dumps((r or {}).get("error", {}))
    check(f"tools/call {builtin} is refused", "Tool not found" in err, err[:90])

send({"jsonrpc":"2.0","id":3,"method":"resources/list"})
r = read()
res = [x.get("name") or x.get("uri") for x in (r or {}).get("result",{}).get("resources",[])]
check("resources/list exposes the vocabulary", len(res) > 0, str(res)[:90])

send({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{
    "name":"semantic_query",
    "arguments":{"metrics":"total_sales","dimensions":"item.i_brand"}}})
r = read()
body = json.dumps((r or {}).get("result", {}))
# Assert on the column names we asked for and on there being a data row beneath the header.
# Asserting on brand names would couple this to whatever dsdgen happens to generate.
returned_columns = "item.i_brand" in body and "total_sales" in body
has_data_row = body.count("|") > 6
check("semantic_query returns rows for the requested columns",
      returned_columns and has_data_row, body[:110])

send({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{
    "name":"semantic_query",
    "arguments":{"metrics":"store_productivity"}}})
r = read()
body = json.dumps((r or {}).get("result", {})) + json.dumps((r or {}).get("error", {}))
check("a refusal reaches the agent as a message", "grain" in body.lower(), body[:110])

proc.terminate()
try:
    proc.wait(timeout=10)
except subprocess.TimeoutExpired:
    proc.kill()

# ---------------------------------------------------------------------------------------------
# Control: the same server WITHOUT the disabling flags must still expose the built-ins.
#
# Without this, every assertion above also passes if duckdb_mcp simply stopped publishing built-in
# tools for some unrelated reason -- we would be green because the threat disappeared, not because
# server.sql closed it, and would never learn our flags had stopped being read. Assert both
# directions. No dsdgen here, so this costs about a second.
control_sql = """
INSTALL duckdb_mcp FROM community; LOAD duckdb_mcp;
PRAGMA mcp_publish_tool('probe_tool','control','SELECT 1 AS x','{}','[]','markdown');
PRAGMA mcp_server_start('stdio');
"""
fd, control_path = tempfile.mkstemp(suffix=".sql", prefix="ossie_mcp_control_")
os.write(fd, control_sql.encode()); os.close(fd)
try:
    proc = subprocess.Popen(
        ["duckdb", "-init", control_path],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, bufsize=1)
    send({"jsonrpc":"2.0","id":1,"method":"initialize","params":{
        "protocolVersion":"2024-11-05","capabilities":{},
        "clientInfo":{"name":"ossie-probe-control","version":"1"}}})
    read()
    send({"jsonrpc":"2.0","id":2,"method":"tools/list"})
    r = read()
    ctl = [t["name"] for t in (r or {}).get("result",{}).get("tools",[])]
    check("control: without the flags the built-ins ARE exposed",
          "query" in ctl and "export" in ctl, str(ctl))
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
finally:
    os.unlink(control_path)

print(f"\n{'all MCP checks passed' if ok else 'MCP checks FAILED'}")
sys.exit(0 if ok else 1)
