PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=ossie
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Override extension-ci-tools' tidy-check: its file regex '$(PROJ_DIR)src/.*/' requires a second
# path separator after src/, so src/ossie_extension.cpp -- the only translation unit directly in
# src/ -- was never linted, while src/model/, src/compile/ and src/functions/ were. run-clang-tidy.py
# gates each file on re.search against that pattern, so the trailing slash is load-bearing. This
# copy widens it to the whole src/ tree. Make prints an "overriding recipe" warning for this target
# on every run; that is expected. Drop the override once the pattern is fixed upstream
# (duckdb/extension-ci-tools#393).
tidy-check:
	mkdir -p ./build/tidy
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) -DDISABLE_UNITY=1 -DCLANG_TIDY=1 -S $(DUCKDB_SRCDIR) -B build/tidy
	cp duckdb/.clang-tidy build/tidy/.clang-tidy
	cd build/tidy && python3 ../../duckdb/scripts/run-clang-tidy.py '$(PROJ_DIR)src/' -header-filter '$(PROJ_DIR)src/' -quiet ${TIDY_THREAD_PARAMETER} ${TIDY_BINARY_PARAMETER} ${TIDY_PERFORM_CHECKS}
