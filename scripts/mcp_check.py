"""End-to-end check of the MCP surface.

Speaks JSON-RPC over stdio to `duckdb -unsigned -init examples/server.sql`, exactly as Claude Desktop
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
import json, subprocess, sys, time

proc = subprocess.Popen(
    ["duckdb", "-unsigned", "-init", "examples/server.sql"],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    text=True, bufsize=1)

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
check("tools/list exposes semantic_query", "semantic_query" in tools, str(tools))

send({"jsonrpc":"2.0","id":3,"method":"resources/list"})
r = read()
res = [x.get("name") or x.get("uri") for x in (r or {}).get("result",{}).get("resources",[])]
check("resources/list exposes the vocabulary", len(res) > 0, str(res)[:90])

send({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{
    "name":"semantic_query",
    "arguments":{"metrics":"total_sales","dimensions":"item.i_brand"}}})
r = read()
body = json.dumps((r or {}).get("result", {}))
check("semantic_query returns real rows", "amalg" in body.lower() or "brand" in body.lower(),
      body[:110])

send({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{
    "name":"semantic_query",
    "arguments":{"metrics":"store_productivity"}}})
r = read()
body = json.dumps((r or {}).get("result", {})) + json.dumps((r or {}).get("error", {}))
check("a refusal reaches the agent as a message", "grain" in body.lower(), body[:110])

proc.terminate()
sys.exit(0 if ok else 1)
