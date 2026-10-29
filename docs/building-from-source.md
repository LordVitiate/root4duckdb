# Building from source

ROOT4DuckDB uses a staged build pipeline:

1. prepare the pinned DuckDB source tree;
2. build Apache Iceberg C++ as a shared runtime;
3. build DuckDB and the ROOT extension against that runtime.

Apache Iceberg does not become part of the extension source tree and is not rebuilt after ordinary ROOT4DuckDB source changes.

## Supported environment

The validated build target is Linux x86_64, including CERN lxplus.

Other operating systems and architectures may work, but they are not currently part of the supported build matrix.

## Requirements

- CERN ROOT with a working `root-config`;
- GCC 14+ or Clang 18+;
- CMake 3.25+;
- Git;
- Python 3;
- Make;
- Ninja or `ninja-build`;
- SQLite development headers;
- `nlohmann/json.hpp`;
- network access during the first dependency fetch.

The build scripts can discover compatible CERN CVMFS toolchains when the system compiler is too old.

Use four parallel jobs on memory-constrained lxplus nodes. Larger values can exhaust the available memory.

## Prepare ROOT

If `root-config` is already available, no additional setup is required:

```bash
root-config --version
```

Otherwise, activate the required ROOT installation:

```bash
source /path/to/root/bin/thisroot.sh
```

A ROOT prefix can also be passed directly to the extension build:

```bash
./build-root4duckdb.sh --root /path/to/root --jobs 4
```

## Prepare the source tree

The source archive does not need Git submodule metadata.

Prepare the pinned DuckDB and extension tool revisions for this source checkout (DuckDB 2.0.0 on `dev`):

```bash
./setup-source-tree.sh
```

The script creates or updates:

```text
duckdb/
extension-ci-tools/
```

Both repositories are checked out at pinned commits. This keeps builds reproducible across machines.

The initial command may appear quiet while the build environment is being detected or dependencies are fetched. Subsequent runs reuse the prepared trees.

This step is also performed automatically by `build-root4duckdb.sh` when either dependency tree is missing.

## Build Apache Iceberg C++

Build and install the shared Iceberg runtime:

```bash
./build-iceberg.sh --jobs 4
```

The runtime is installed under:

```text
.deps/iceberg-cpp/
```

The build produces separate shared libraries, including:

```text
libiceberg.so
libiceberg_data.so
libiceberg_bundle.so
libiceberg_sql_catalog.so
```

ROOT4DuckDB links to these libraries dynamically.

Use `--clean` when the compiler, standard library or Iceberg configuration changes:

```bash
./build-iceberg.sh --clean --jobs 4
```

To select a compiler explicitly:

```bash
CC=/path/to/gcc \
CXX=/path/to/g++ \
./build-iceberg.sh --clean --jobs 4
```

The selected compiler is recorded and reused by the ROOT4DuckDB build.

## Build ROOT4DuckDB

Build DuckDB and the extension:

```bash
./build-root4duckdb.sh --jobs 4
```

For a clean build with the complete test suite:

```bash
./build-root4duckdb.sh --clean --tests --jobs 4
```

The build wrapper:

- validates the source layout;
- verifies the serialized codec;
- checks compiler and ROOT ABI compatibility;
- prepares the pinned DuckDB sources when necessary;
- configures the extension build;
- builds the DuckDB CLI and loadable extension;
- checks shared-library resolution;
- verifies the registered SQL functions;
- optionally runs native and SQL integration tests.

## Complete build sequence

For a new source checkout:

```bash
./setup-source-tree.sh
./build-iceberg.sh --jobs 4
./build-root4duckdb.sh --clean --tests --jobs 4
```

`setup-source-tree.sh` may be omitted because the final build command invokes it when required.

## Build outputs

### DuckDB CLI

```text
build/release/duckdb
```

This is the bundled DuckDB 2.0 CLI. ROOT4DuckDB is built as a loadable extension and must be loaded explicitly.

### Loadable extension

```text
build/release/extension/root/root.duckdb_extension
```

This artifact can be loaded into a compatible DuckDB 2.0 process with an appropriate ROOT and C++ runtime:

```sql
LOAD './build/release/extension/root/root.duckdb_extension';
```

### Shared Iceberg runtime

```text
.deps/iceberg-cpp/
```

The bundled DuckDB CLI uses shared Iceberg libraries, whereas the loadable extension embeds Iceberg statically and discovers a compatible ROOT runtime at load time.

## Run

Use the environment-aware launcher:

```bash
./run-duckdb.sh
```

Arguments are forwarded to DuckDB:

```bash
./run-duckdb.sh database.duckdb
```

For a non-interactive smoke test:

```bash
./run-duckdb.sh -unsigned -cmd "LOAD './build/release/extension/root/root.duckdb_extension';" -c "
    SELECT function_name
    FROM duckdb_functions()
    WHERE function_name IN (
        'read_root',
        'read_root_histogram',
        'root_build_index',
        'read_root_dataset',
        'root_iceberg_catalog'
    )
    ORDER BY function_name;
"
```

The executable can also be launched directly when the compiler, ROOT and Iceberg runtime paths are already present:

```bash
./build/release/duckdb
```

On lxplus, prefer `run-duckdb.sh`. It restores the required runtime paths before starting the same binary.

## Incremental rebuilds

Apache Iceberg does not need to be rebuilt after ordinary changes under `src/`.

Rebuild only the changed extension translation units and relink the required artifacts:

```bash
./scripts/rebuild-extension.sh --jobs 4
```

The script also performs a short SQL smoke test.

Skip that smoke test when only compilation is required:

```bash
./scripts/rebuild-extension.sh --jobs 4 --no-smoke
```

Use the full wrapper when CMake configuration, compiler settings, dependency paths or extension registration changes:

```bash
./build-root4duckdb.sh --clean --tests --jobs 4
```

## Tests

Run the complete supported test path through the build wrapper:

```bash
./build-root4duckdb.sh --tests --jobs 4
```

Run the integration suite against an existing build:

```bash
./scripts/run-integration-test.sh
```

Run the serialized codec checks separately:

```bash
./scripts/test-serialized-codec.sh
```

## Doxygen documentation

API documentation is configured by the tracked root-level `Doxyfile`.

Doxygen 1.9.1 or newer is supported. Graphviz is required because the documentation includes SVG class, collaboration, include, call and caller graphs.

### Install the documentation tools

Debian or Ubuntu:

```bash
sudo apt update
sudo apt install doxygen graphviz
```

RHEL-compatible systems:

```bash
sudo dnf install doxygen graphviz
```

Without administrator privileges:

```bash
conda install -c conda-forge doxygen graphviz
```

Verify both programs:

```bash
doxygen --version
dot -V
```

Generate the documentation:

```bash
doxygen Doxyfile
```

Open:

```text
doxygen/html/index.html
```

The generated `doxygen/` directory is intentionally excluded from Git. Only the configuration and documented source code are tracked.

Graph rendering uses one worker thread for compatibility with Doxygen 1.9.1 and to avoid concurrent SVG patching failures.

## Manual CMake rebuilds

Prefer `build-root4duckdb.sh` or `scripts/rebuild-extension.sh`. They restore the compiler and shared-library environment automatically.

When invoking CMake manually, first restore the generated build environment:

```bash
source .deps/build-env.sh
source .deps/iceberg-env.sh
```

Then ensure the selected compiler runtime is visible:

```bash
toolchain_lib="$(dirname "$("$CXX" -print-file-name=libstdc++.so.6)")"
export LD_LIBRARY_PATH="$toolchain_lib:${LD_LIBRARY_PATH:-}"
```

Now the existing build tree can be rebuilt:

```bash
cmake --build build/release --parallel 4
```

## Troubleshooting

### `root-config` is unavailable

Activate ROOT:

```bash
source /path/to/root/bin/thisroot.sh
```

Alternatively:

```bash
./build-root4duckdb.sh --root /path/to/root --jobs 4
```

### ROOT dictionary duplicates or incompatible PCRE2

On lxplus, use one matching LCG view, compiler and ROOT runtime. The current CERN test pairing is LCG_110a with ROOT 6.40.04. Mixing LCG_110 with ROOT 6.40.04 can select a PCRE2 library that lacks the versioned symbols required by libCore.so, causing link failures. A conflicting ROOT runtime or PCM search path can also produce duplicate TClassTable registrations.

Use the detected build environment where possible, or explicitly set ROOT4DUCKDB_LCG_VIEW and ROOT4DUCKDB_ROOT_CONFIG. The integration runner checks the CMake-pinned ROOT library and reports incompatible shared-library dependencies before building fixtures.

### `GLIBCXX_... not found`

The executable is being started with a different `libstdc++` from the one used during compilation.

Use:

```bash
./run-duckdb.sh
```

For manual builds, restore `.deps/build-env.sh` and add the selected compiler runtime to `LD_LIBRARY_PATH`.

### `dot: not found`

Install Graphviz and verify:

```bash
dot -V
```

Doxygen itself can parse the source without Graphviz, but the configured diagrams cannot be generated.

### Doxygen cannot rename an SVG file

Remove the incomplete generated documentation and run Doxygen again after confirming that `dot` is installed:

```bash
rm -rf doxygen
doxygen Doxyfile
```

The tracked source files are not affected.

### Shared Iceberg libraries are missing

Rebuild or verify the shared runtime:

```bash
./build-iceberg.sh --jobs 4
./scripts/check-iceberg.sh
```

### The build tree contains an old compiler configuration

Reconfigure through the supported clean build path:

```bash
./build-root4duckdb.sh --clean --tests --jobs 4
```

### The build is terminated on lxplus

Reduce parallelism:

```bash
./build-root4duckdb.sh --jobs 4
```

The same recommendation applies to Iceberg and incremental extension builds.

## DuckDB Community CI

The official DuckDB distribution workflow is in `.github/workflows/MainDistributionPipeline.yml`. It targets DuckDB 2.0.0 on Linux x86_64 only and is independent of the CERN release-package path.

When DuckDB's Linux container sets `LINUX_CI_IN_DOCKER=1`, `make release` first runs `scripts/prepare-community-deps.sh`. This installs CERN ROOT 6.40.04 and GCC 14 via conda-forge into `.deps/community-root/`, then builds the pinned Apache Iceberg C++ into `.deps/iceberg-cpp/`. The resulting extension uses the runtime bootstrap and keeps CERN ROOT external.

The equivalent opt-in for a manual build is `ROOT4DUCKDB_COMMUNITY_BUILD=1 make release` after preparing the pinned DuckDB and CI-tool sources. No changes to the regular CERN build are needed.

A GitHub Actions build must finish successfully before publishing. Registration is a separate pull request adding `extensions/root/description.yml` to `duckdb/community-extensions`, referring to a public commit of `LordVitiate/root4duckdb`. The private `root4duckdb-dev` repository cannot be used as the Community source. A matching CERN ROOT installation is still required when the user loads the extension.
