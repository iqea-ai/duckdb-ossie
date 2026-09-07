-- Publish an Ossie semantic model to AI agents over MCP.
--   duckdb -init examples/server.sql
--
-- Point Claude Desktop at it with:
--   {"mcpServers": {"ossie": {"command": "duckdb",
--                             "args": ["-init", "/abs/path/to/server.sql"]}}}

-- Every extension this script uses must be loaded explicitly. Running it from a build tree hides
-- that, because ossie and tpcds are statically linked there; a user installing from the registry
-- gets neither unless it is spelled out.
INSTALL ossie FROM community;
LOAD ossie;

INSTALL duckdb_mcp FROM community;
LOAD duckdb_mcp;

-- tpcds only supplies dsdgen, for the demo data below. Drop both lines when pointing this at
-- your own tables.
INSTALL tpcds;
LOAD tpcds;

-- Replace with your own data. dsdgen gives a runnable demo out of the box.
CALL dsdgen(sf = 0.01);

-- allow_filter_functions stays off: the agent can only reach the model's own vocabulary.
CALL ossie_load('test/fixtures/tpcds_semantic_model.json',
                rebind => MAP{'tpcds.public': 'memory.main'});

PRAGMA mcp_publish_query('metrics', 'SELECT name, description, synonyms FROM ossie_metrics()');
PRAGMA mcp_publish_query('dimensions', 'SELECT dataset, name, datatype, synonyms FROM ossie_fields()');

PRAGMA mcp_publish_tool(
    'semantic_query',
    'Answer a question against the semantic model. Call the metrics and dimensions tools first and
     use exact names. metrics and dimensions are comma-separated; filters are semicolon-separated
     SQL predicates over dataset.field names, e.g. date_dim.d_year = 2001',
    'SELECT * FROM ossie_query(
         string_split($metrics, '','') ,
         CASE WHEN $dimensions IS NULL THEN []::VARCHAR[] ELSE string_split($dimensions, '','') END,
         CASE WHEN $filters IS NULL THEN []::VARCHAR[] ELSE string_split($filters, '';'') END)',
    '{"metrics":    {"type": "string", "description": "Comma-separated metric names"},
      "dimensions": {"type": "string", "description": "Comma-separated dataset.field names"},
      "filters":    {"type": "string", "description": "Semicolon-separated SQL predicates"}}',
    '["metrics"]',
    'markdown'
);

-- SECURITY: the five flags below are what make that guarantee hold. duckdb_mcp publishes generic
-- tools -- query, export, list_tables, describe, database_info -- alongside anything you publish,
-- and each one reaches the whole database. Disabling query alone is not enough: export takes an
-- arbitrary SQL argument and is a second route to the same place. With all five off, tools/list
-- returns only semantic_query and tools/call on the others is refused with "Tool not found".
--
-- Remove a flag and you reopen the connection. scripts/mcp_check.py asserts both directions of this
-- on every push, so CI fails if it stops being true.
PRAGMA mcp_server_start('stdio', 'localhost', 0, '{
    "enable_query_tool": false,
    "enable_export_tool": false,
    "enable_describe_tool": false,
    "enable_list_tables_tool": false,
    "enable_database_info_tool": false
}');
