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

All seven files are taken at apache/ossie commit `8dd6732da354f22ca71f16d82626a46039ecdcd9` and sit
under a directory named for the converter that wrote them, keeping upstream's own filenames, so where
each came from is visible in its path. (Omni's file now carries the name the old Databricks fixture had;
the directory keeps the two from being confused in history.)

| file | source in apache/ossie | loads today |
|---|---|---|
| `orionbelt/tpcds_ossie.yaml` | `converters/orionbelt/tests/fixtures/` | yes |
| `omni/fixtureA_ossie.yaml` | `converters/omni/tests/fixtures/` | yes |
| `gooddata/ossie_tpcds.yaml` | `converters/gooddata/tests/fixtures/` | yes |
| `nvidia/sales.ossie.yaml` | `converters/nvidia/tests/fixtures/` | yes (its one metric is multi-grain, so queries on it are refused) |
| `databricks/ossie_fixtureA_ossie.yaml` | `converters/databricks/java/src/test/resources/` | no — carries only `DATABRICKS` expressions |
| `examples/tpcds_semantic_model.yaml` | `examples/` | yes (its three window-function metrics are refused when queried) |

`ossie-schema.json` is the official schema from `core-spec/`, kept so the models here (and our own)
can be validated against the format's own definition rather than against our reading of it.

All seven are in the flat document shape that Ossie 0.2.0.dev0 requires: one model at the root, no
`semantic_model` array. When upstream changes them again, re-vendor all of them from one commit and
update that commit here and in `NOTICE`.

Five of six models load. The one refusal is correct: that model carries only `DATABRICKS` expressions, so
this extension genuinely cannot execute it.

The gooddata and nvidia models were refused until the declared-field requirements were removed from
`validate.cpp` — both are valid against the official schema, which requires neither that a
relationship's columns be declared fields nor that a field expression reference only declared
fields. Keep this table and `test/sql/ossie_conformance.test` in step when the number changes.
