# Conformance fixtures

Ossie models written by **other implementers**, copied verbatim from
[apache/ossie](https://github.com/apache/ossie). They are Apache-2.0 licensed; see that repository
for terms and authorship.

They are here because every other fixture in `test/fixtures/` was written in this repository, by the
same person who wrote the compiler. Those fixtures answer "does the compiler agree with itself?"
They cannot answer "does it agree with the Ossie format?", because the fixtures and the compiler
share the same assumptions. These files are the only ones that can.

They are **copied in, not fetched at test time**: CI must not depend on the network, and upstream
moves.

| file | source in apache/ossie | loads today |
|---|---|---|
| `tpcds_osi.yaml` | `converters/orionbelt/tests/fixtures/` | yes |
| `fixtureA_osi.yaml` | `converters/omni/tests/fixtures/` | yes |
| `osi_tpcds.yaml` | `converters/gooddata/tests/fixtures/` | yes |
| `sales.ossie.yaml` | `converters/nvidia/tests/fixtures/` | yes (its one metric is multi-grain, so queries on it are refused) |
| `fixtureA_ossie.yaml` | `converters/databricks/tests/fixtures/` | no — carries only `DATABRICKS` expressions |

`osi-schema.json` is the official `core-spec/osi-schema.json`, kept so the models here (and our own)
can be validated against the format's own definition rather than against our reading of it.

Four of five load. The one refusal is correct: that model carries only `DATABRICKS` expressions, so
this extension genuinely cannot execute it.

The gooddata and nvidia models were refused until the declared-field requirements were removed from
`validate.cpp` — both are valid against the official schema, which requires neither that a
relationship's columns be declared fields nor that a field expression reference only declared
fields. Keep this table and `test/sql/ossie_conformance.test` in step when the number changes.
