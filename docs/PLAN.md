# Plan: Factor out dta into a separate extension

## Context

The .dta reader/writer in `src/dta/` should live in its own repo (`codedthinking/duckdb-dta`) so it can be:
1. A standalone DuckDB extension (`INSTALL dta FROM community`)
2. A compile-time dependency of dodo (via git submodule)

This avoids maintaining code in two places — dodo links to the dta source at build time.

## New repo: `codedthinking/duckdb-dta`

### Structure

```
duckdb-dta/
├── .github/workflows/        (from extension-ci-tools)
├── .gitmodules                (duckdb + extension-ci-tools submodules)
├── CMakeLists.txt
├── Makefile
├── extension_config.cmake
├── vcpkg.json
├── src/
│   ├── include/
│   │   ├── dta_reader.hpp
│   │   └── dta_writer.hpp
│   ├── dta_extension.cpp      (LoadInternal: registers read_dta + CopyFunction)
│   ├── dta_reader.cpp
│   ├── dta_writer.cpp
│   ├── read_dta_function.hpp
│   ├── read_dta_function.cpp
│   ├── write_dta_function.hpp
│   └── write_dta_function.cpp
└── test/
    ├── data/auto.dta
    └── sql/
        ├── read_dta.test
        └── write_dta.test
```

### Key files

**extension_config.cmake:**
```cmake
duckdb_extension_load(dta
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
```

**CMakeLists.txt:**
```cmake
cmake_minimum_required(VERSION 3.5)
set(TARGET_NAME dta)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME})

include_directories(src/include)

set(EXTENSION_SOURCES
    src/dta_extension.cpp
    src/dta_reader.cpp
    src/dta_writer.cpp
    src/read_dta_function.cpp
    src/write_dta_function.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})
```

**src/dta_extension.cpp** — new file, thin loader:
```cpp
#include "read_dta_function.hpp"
#include "write_dta_function.hpp"
#include "duckdb.hpp"

namespace duckdb {
static void LoadInternal(ExtensionLoader &loader) {
    loader.RegisterFunction(GetReadDtaFunction());
    loader.RegisterFunction(GetDtaCopyFunction());
}
// ... standard extension boilerplate
}
```

### What moves from dodo

| From (dodo) | To (duckdb-dta) |
|---|---|
| `src/dta/dta_reader.hpp` | `src/include/dta_reader.hpp` |
| `src/dta/dta_reader.cpp` | `src/dta_reader.cpp` |
| `src/dta/dta_writer.hpp` | `src/include/dta_writer.hpp` |
| `src/dta/dta_writer.cpp` | `src/dta_writer.cpp` |
| `src/dta/read_dta_function.hpp` | `src/read_dta_function.hpp` |
| `src/dta/read_dta_function.cpp` | `src/read_dta_function.cpp` |
| `src/dta/write_dta_function.hpp` | `src/write_dta_function.hpp` |
| `src/dta/write_dta_function.cpp` | `src/write_dta_function.cpp` |
| `test/data/auto.dta` | `test/data/auto.dta` |
| `test/sql/read_dta.test` | `test/sql/read_dta.test` |
| `test/sql/write_dta.test` | `test/sql/write_dta.test` (stripped of dodo-specific tests) |

## Changes to dodo repo

### 1. Add submodule

```bash
git submodule add https://github.com/codedthinking/duckdb-dta duckdb-dta
```

### 2. Update `extension_config.cmake`

```cmake
# Load dta extension first (dependency)
duckdb_extension_load(dta
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/duckdb-dta
)

# Then load dodo
duckdb_extension_load(dodo
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
```

### 3. Update `CMakeLists.txt`

Remove dta source files, include dta headers:

```cmake
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/src/extension
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/core
                    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb-dta/src/include
                    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb-dta/src)

set(EXTENSION_SOURCES
    src/extension/dodo_extension.cpp
    src/core/dodo_core.cpp)
```

### 4. Update `dodo_extension.cpp`

Remove `read_dta_function.hpp` and `write_dta_function.hpp` includes, remove the `RegisterFunction` calls for read_dta and CopyFunction — those are now registered by the dta extension's own LoadInternal.

Keep the `#include "dta_reader.hpp"` in `dodo_core.cpp` (for variable label extraction on `use`).

### 5. Update `dodoc` CLI target

```cmake
add_executable(dodoc src/cli/dodoc.cpp src/core/dodo_core.cpp
               duckdb-dta/src/dta_reader.cpp)
target_include_directories(dodoc PRIVATE src/core duckdb-dta/src/include)
```

### 6. Delete `src/dta/` directory

All files moved to the new repo.

## Split of write_dta.test

The current `test/sql/write_dta.test` has dodo-specific tests (save command, variable labels via codebook). Split into:

- **duckdb-dta** tests: pure SQL (`COPY TO`, `read_dta`, type mapping, round-trip)
- **dodo** tests: dodo-specific (`save "file.dta"`, `codebook`, variable label round-trip) — stays in dodo repo as part of `dodo.test` or a new `test/sql/dodo_dta.test`

## Implementation order

1. Create `codedthinking/duckdb-dta` repo from extension template
2. Copy files from `src/dta/` to new repo, restructure (headers to `src/include/`)
3. Add `dta_extension.cpp` boilerplate
4. Verify standalone build: `make` in duckdb-dta
5. Verify standalone tests: `make test` in duckdb-dta
6. Add duckdb-dta as submodule in dodo
7. Update dodo's `extension_config.cmake`, `CMakeLists.txt`, `dodo_extension.cpp`
8. Delete `src/dta/` from dodo
9. Verify dodo build: `make` in dodo
10. Verify dodo tests: `make test` in dodo

## Verification

1. **Standalone dta**: `cd duckdb-dta && make && make test`
2. **Standalone dta SQL**: `./build/release/duckdb -c "SELECT * FROM read_dta('test/data/auto.dta')"`
3. **dodo with dta**: `cd dodo && make && make test` (all 1168+ assertions)
4. **dodo save .dta**: `use "test/data/auto.dta", clear; save "/tmp/out.dta"; clear; use "/tmp/out.dta", clear; codebook`
