PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

EXT_NAME := root
EXT_CONFIG := ${PROJ_DIR}extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Drop extension-ci options unused by the pinned DuckDB build.
BUILD_FLAGS := $(filter-out -DBENCHMARK_ROOT_DIRECTORY=% -DBUILD_EXTENSION_TEST_DEPS=%,$(BUILD_FLAGS))

.PHONY: duckdb-source-for-ci
duckdb-source-for-ci:
	@if [ ! -d duckdb/.git ]; then \
		if [ -e duckdb ]; then \
			echo "[ERROR] duckdb/ exists but is not a Git checkout" >&2; exit 1; \
		fi; \
		git clone --filter=blob:none --depth 1 --branch "$(DUCKDB_GIT_VERSION)" \
			https://github.com/duckdb/duckdb.git duckdb; \
	fi

set_duckdb_version: duckdb-source-for-ci


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

ifneq ($(filter 1,$(ROOT4DUCKDB_COMMUNITY_BUILD) $(LINUX_CI_IN_DOCKER)),)
COMMUNITY_PREFIX := $(PROJ_DIR).deps/community-root
COMMUNITY_TOOLS := $(PROJ_DIR).deps/community-tools
export ROOT4DUCKDB_DISABLE_PINNED_LCG := 1
export ROOT4DUCKDB_ROOT_CONFIG := $(COMMUNITY_PREFIX)/bin/root-config
export ROOT4DUCKDB_ICEBERG_PREFIX := $(PROJ_DIR).deps/iceberg-cpp
export CC := $(COMMUNITY_PREFIX)/bin/x86_64-conda-linux-gnu-gcc
export CXX := $(COMMUNITY_PREFIX)/bin/x86_64-conda-linux-gnu-g++
export PATH := $(COMMUNITY_TOOLS):$(COMMUNITY_PREFIX)/bin:$(PATH)
export CMAKE_PREFIX_PATH := $(COMMUNITY_PREFIX)$(if $(CMAKE_PREFIX_PATH),:$(CMAKE_PREFIX_PATH))
export LD_LIBRARY_PATH := $(COMMUNITY_PREFIX)/lib$(if $(LD_LIBRARY_PATH),:$(LD_LIBRARY_PATH))
EXT_RELEASE_FLAGS += -DROOT4DUCKDB_RUNTIME_BOOTSTRAP=ON -DROOT4DUCKDB_LOADABLE_ONLY=ON
EXT_DEBUG_FLAGS += -DROOT4DUCKDB_RUNTIME_BOOTSTRAP=ON -DROOT4DUCKDB_LOADABLE_ONLY=ON

.PHONY: community-prepare
community-prepare:
	bash scripts/prepare-community-deps.sh

release debug reldebug: community-prepare
endif
