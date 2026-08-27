# Extension updating 
When cloning this template, the target version of DuckDB should be the latest stable release of DuckDB. However, there 
will inevitably come a time when a new DuckDB is released and the extension repository needs updating. This process goes
as follows:

- Bump submodules
  - `./duckdb` should be set to latest tagged release
  - `./extension-ci-tools` should be set to updated branch corresponding to latest DuckDB release. So if you're building for DuckDB `v1.1.0` there will be a branch in `extension-ci-tools` named `v1.1.0` to which you should check out. 
- Bump every version reference in `.github/workflows/MainDistributionPipeline.yml`. There are six,
  and they must all name the same release, or CI and a local `make` build against different trees:
  - `duckdb_version` and `ci_tools_version` in both the `duckdb-stable-build` and
    `code-quality-check` jobs
  - the `@ref` on both reusable workflows (`_extension_distribution.yml` and
    `_extension_code_quality.yml`)
- Record the new submodule commits in the comment block at the top of `.gitmodules`, and update the
  DuckDB version stated in `README.md`
- Note that extension-ci-tools publishes branches, not tags, and a branch keeps moving after you pin
  it. Prefer a patch branch such as `v1.5.5` over a release-line branch such as `v1.5-variegata`.

# API changes
DuckDB extensions built with this extension template are built against the internal C++ API of DuckDB. This API is not guaranteed to be stable.
What this means for extension development is that when updating your extensions DuckDB target version using the above steps, you may run into the fact that your extension no longer builds properly.

Currently, DuckDB does not (yet) provide a specific change log for these API changes, but it is generally not too hard to figure out what has changed.

For figuring out how and why the C++ API changed, we recommend using the following resources:
- DuckDB's [Release Notes](https://github.com/duckdb/duckdb/releases)
- DuckDB's history of [Core extension patches](https://github.com/duckdb/duckdb/commits/main/.github/patches/extensions)
- The git history of the relevant C++ Header file of the API that has changed