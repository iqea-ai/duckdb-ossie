-- Publish an Ossie semantic model to AI agents over MCP.
--   duckdb -unsigned -init examples/server.sql
--
-- Point Claude Desktop at it with:
--   {"mcpServers": {"ossie": {"command": "duckdb",
--                             "args": ["-unsigned", "-init", "/abs/path/to/server.sql"]}}}

INSTALL duckdb_mcp FROM community;
LOAD duckdb_mcp;

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

PRAGMA mcp_server_start('stdio');
