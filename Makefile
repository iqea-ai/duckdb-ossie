PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=ossie
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Override extension-ci-tools' tidy-check.
#
# WHAT IS BROKEN UPSTREAM: the file regex it passes to run-clang-tidy.py is
# '$(PROJ_DIR)src/.*/'. That pattern is matched with re.search, and the trailing slash requires a
# second path separator after src/ -- so any translation unit sitting directly in src/ can never
# match and is silently never linted. Here that is src/ossie_extension.cpp; src/model/,
# src/compile/ and src/functions/ were always covered. This copy widens the pattern to the whole
# src/ tree.
#
# EXPECTED NOISE: make prints an "overriding recipe for target 'tidy-check'" warning on every run
# because this redefines a target the included makefile already defines. That is expected and
# harmless.
#
# DELETE CONDITION: drop this override when duckdb/extension-ci-tools#393 has landed *in the
# ci-tools ref this repo pins* -- not merely on upstream main. The fix being merged upstream does
# not help until .gitmodules and the workflow move to a ref that contains it. Still OPEN as of
# 2026-08-27.
#
# Note the authoritative tidy environment is CI, not a developer machine: the tidy-check job in
# _extension_code_quality.yml runs distro clang-tidy on ubuntu-24.04 with DuckDB's .clang-tidy
# config. A local run on macOS against homebrew clang-tidy fails to find the SDK headers and proves
# nothing either way.
tidy-check:
	mkdir -p ./build/tidy
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) -DDISABLE_UNITY=1 -DCLANG_TIDY=1 -S $(DUCKDB_SRCDIR) -B build/tidy
	cp duckdb/.clang-tidy build/tidy/.clang-tidy
	cd build/tidy && python3 ../../duckdb/scripts/run-clang-tidy.py '$(PROJ_DIR)src/' -header-filter '$(PROJ_DIR)src/' -quiet ${TIDY_THREAD_PARAMETER} ${TIDY_BINARY_PARAMETER} ${TIDY_PERFORM_CHECKS}
