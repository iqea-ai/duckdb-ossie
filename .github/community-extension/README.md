# Community-extensions descriptor

`description.yml` is a **staging copy**. It does not do anything here.

To publish, it is copied to `extensions/ossie/description.yml` in a pull request against
[duckdb/community-extensions](https://github.com/duckdb/community-extensions). Keeping a copy in
this repo means the wording stays under review with the code that it describes, rather than being
written once in a PR nobody here sees again.

Before opening that PR:

1. Set `repo.ref` to the **full 40-character commit SHA** of the release tag. Never a tag name or a
   branch -- the registry pins a commit.
2. Check `extension.version` matches the tag and the top entry in `CHANGELOG.md`. This is the one
   field with no automated check, and it drifted on duckdb-snowflake (descriptor said 0.4.1 while
   the tags had reached 0.5.2).
3. Confirm the claims are still true. The conformance paragraph states four of five upstream models
   load; `test/sql/ossie_conformance.test` is what makes that checkable.

No `install_notes` are needed. The extension has no runtime dependencies -- which is deliberate, and
is why the `yaml` community extension was rejected as a YAML parser in favour of a pinned submodule.
See `third_party/fkYAML/README.md`.
