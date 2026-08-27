PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

EXT_NAME := root
EXT_CONFIG := ${PROJ_DIR}extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Drop extension-ci options unused by DuckDB 1.4.5.
BUILD_FLAGS := $(filter-out -DBENCHMARK_ROOT_DIRECTORY=% -DBUILD_EXTENSION_TEST_DEPS=%,$(BUILD_FLAGS))

.PHONY: release-package
release-package: ${EXTENSION_CONFIG_STEP}
	cmake -E make_directory build/release-package
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) \
		-DCMAKE_BUILD_TYPE=Release \
		-DROOT4DUCKDB_LOADABLE_ONLY=ON \
		-S $(DUCKDB_SRCDIR) \
		-B build/release-package
	cmake --build build/release-package --config Release \
		--target root_loadable_extension root_loadable_extension_smoke
