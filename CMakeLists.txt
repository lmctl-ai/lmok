cmake_minimum_required(VERSION 3.24)
project(lmok VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(LMOK_FULL_STATIC "Link the complete executable statically" ON)
set(LMOK_HOST_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/schema/lmok.host.json")

# ---- vendored tree-sitter ---------------------------------------------------
# third_party/tree-sitter, third_party/tree-sitter-json,
# third_party/tree-sitter-c, third_party/tree-sitter-python,
# third_party/tree-sitter-bash, tree-sitter-go, tree-sitter-javascript and tree-sitter-typescript are git-ignored and synced by
# scripts/sync-deps.sh, pinned by deps.lock. The C library is compiled through
# its amalgamation lib/src/lib.c alone (which #includes the individual
# translation units); each grammar contributes src/parser.c plus its
# src/scanner.c external scanner where one exists at the pinned commit
# (python, bash, javascript, typescript, lmscript and lmcode; json, c, java and go have none).
set(TS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter")
set(TSJSON_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-json")
set(TSC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-c")
set(TSPY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-python")
set(TSBASH_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-bash")
set(TSGO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-go")
set(TSJS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-javascript")
set(TSTS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-typescript")

set(TSJAVA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-java")

set(TSLMS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-lmscript")
set(TSLMC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tree-sitter-lmcode")

set(TS_SOURCES
  "${TSLMS_ROOT}/src/parser.c"
  "${TSLMS_ROOT}/src/scanner.c"
  "${TSLMC_ROOT}/src/parser.c"
  "${TSLMC_ROOT}/src/scanner.c"
  "${TSJAVA_ROOT}/src/parser.c"
  "${TS_ROOT}/lib/src/lib.c"
  "${TSJSON_ROOT}/src/parser.c"
  "${TSC_ROOT}/src/parser.c"
  "${TSPY_ROOT}/src/parser.c"
  "${TSPY_ROOT}/src/scanner.c"
  "${TSBASH_ROOT}/src/parser.c"
  "${TSBASH_ROOT}/src/scanner.c"
  "${TSGO_ROOT}/src/parser.c"
  "${TSJS_ROOT}/src/parser.c"
  "${TSJS_ROOT}/src/scanner.c"
  "${TSTS_ROOT}/typescript/src/parser.c"
  "${TSTS_ROOT}/typescript/src/scanner.c"
  "${TSTS_ROOT}/tsx/src/parser.c"
  "${TSTS_ROOT}/tsx/src/scanner.c"
)

# ---- embedded DSL schema and notices ---------------------------------------
set(SCHEMA_JSON "${CMAKE_CURRENT_SOURCE_DIR}/schema/lmok.dsl.json")
# Merge the pinned SDK catalog explicitly: during compiler/SDK rollouts the
# installed compiler's implicit standard may predate the host's return imports.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${LMOK_HOST_SCHEMA}" "${CMAKE_CURRENT_SOURCE_DIR}/cpp/std/lmcode-std.schema.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/merge-std.py")
execute_process(COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/scripts/merge-std.py" "${LMOK_HOST_SCHEMA}" "${SCHEMA_JSON}.tmp"
  RESULT_VARIABLE SCHEMA_MERGE_RESULT)
if(NOT SCHEMA_MERGE_RESULT EQUAL 0)
  message(FATAL_ERROR "Cannot merge lmok host schema with pinned canonical std")
endif()
# Preserve the generated header's timestamp when reconfiguring identical bytes.
configure_file("${SCHEMA_JSON}.tmp" "${SCHEMA_JSON}" COPYONLY)
file(REMOVE "${SCHEMA_JSON}.tmp")
set(GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(GENERATED_SCHEMA "${GENERATED_DIR}/schema_embedded.hpp")
set(GENERATED_NOTICES "${GENERATED_DIR}/notices_embedded.hpp")
add_custom_command(
  OUTPUT "${GENERATED_SCHEMA}"
  COMMAND "${CMAKE_COMMAND}"
    -DINPUT="${SCHEMA_JSON}"
    -DOUTPUT="${GENERATED_SCHEMA}"
    -DNAME=embedded_schema
    -DCOMMENT=schema/lmok.dsl.json
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake"
  DEPENDS "${SCHEMA_JSON}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake"
  COMMENT "Embedding schema/lmok.dsl.json"
)
add_custom_command(
  OUTPUT "${GENERATED_NOTICES}"
  COMMAND "${CMAKE_COMMAND}"
    -DINPUT="${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
    -DOUTPUT="${GENERATED_NOTICES}"
    -DNAME=embedded_notices
    -DCOMMENT=THIRD_PARTY_NOTICES.md
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake"
  COMMENT "Embedding THIRD_PARTY_NOTICES.md"
)
# ---- embedded facts.scm query files (E6, cpp/facts.cpp) ---------------------
set(GENERATED_FACTS)
foreach(lang c python bash json java go javascript typescript tsx lmscript lmcode)
  set(_facts_out "${GENERATED_DIR}/facts_${lang}_embedded.hpp")
  add_custom_command(
    OUTPUT "${_facts_out}"
    COMMAND "${CMAKE_COMMAND}"
      -DINPUT="${CMAKE_CURRENT_SOURCE_DIR}/queries/${lang}/facts.scm"
      -DOUTPUT="${_facts_out}"
      -DNAME=embedded_facts_${lang}
      -DCOMMENT=queries/${lang}/facts.scm
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/queries/${lang}/facts.scm" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_schema.cmake"
    COMMENT "Embedding queries/${lang}/facts.scm"
  )
  list(APPEND GENERATED_FACTS "${_facts_out}")
endforeach()
add_custom_target(lmok_schema DEPENDS "${GENERATED_SCHEMA}" "${GENERATED_NOTICES}" ${GENERATED_FACTS})

add_library(lmok_sqlite OBJECT third_party/sqlite/sqlite3.c)

add_executable(lmok
  cpp/main.cpp
  cpp/host.cpp
  cpp/lint.cpp
  cpp/pattern.cpp
  cpp/db.cpp
  cpp/search.cpp
  cpp/baseline.cpp
  cpp/sarif.cpp
  $<TARGET_OBJECTS:lmok_sqlite>
  cpp/facts.cpp
  cpp/metrics.cpp
  ${TS_SOURCES}
  "${GENERATED_SCHEMA}"
  "${GENERATED_NOTICES}"
  ${GENERATED_FACTS}
)
add_dependencies(lmok lmok_schema)

target_include_directories(lmok PRIVATE
  cpp
  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite"
  "${TS_ROOT}/lib/include"
  "${TS_ROOT}/lib/src"
  "${TSTS_ROOT}/typescript/src"
  "${TSTS_ROOT}/tsx/src"
  "${GENERATED_DIR}"
)
# Same feature-test defines as the upstream tree-sitter Makefile.
target_compile_definitions(lmok PRIVATE _POSIX_C_SOURCE=200112L _DEFAULT_SOURCE)
# tree-sitter sources are third-party: silence their warnings, keep ours strict.
set_source_files_properties(${TS_SOURCES} PROPERTIES
  COMPILE_OPTIONS "-w"
)
target_compile_options(lmok PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(lmok PRIVATE LMOK_VERSION="${PROJECT_VERSION}")

# Static linking: no runtime dependency beyond the OS C library.
# ("Clang" also matches AppleClang; macOS has no -static/-static-libstdc++.)
if((CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang") AND NOT APPLE)
  target_link_options(lmok PRIVATE -static-libstdc++ -static-libgcc)
  if(LMOK_FULL_STATIC)
    target_link_options(lmok PRIVATE -static)
  endif()
endif()

# Use UTF-16 CRT arguments on MinGW, then convert once to the host UTF-8 API.
if(MINGW)
  target_link_options(lmok PRIVATE -municode)
endif()

# SQLite is embedded, with FTS5 and serialized connections. Disable extension
# loading so a static binary never dlopens a runtime library.
set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES
  COMPILE_DEFINITIONS "SQLITE_ENABLE_FTS5;SQLITE_THREADSAFE=1;SQLITE_OMIT_LOAD_EXTENSION"
  COMPILE_OPTIONS "-w"
)
find_package(Threads REQUIRED)
target_link_libraries(lmok PRIVATE Threads::Threads)
if(UNIX)
  target_link_libraries(lmok PRIVATE m)
endif()

# Test helper is excluded from the default single-CLI build.
add_executable(lmok_db_test EXCLUDE_FROM_ALL tests/db-wrapper.cpp cpp/db.cpp $<TARGET_OBJECTS:lmok_sqlite>)
target_include_directories(lmok_db_test PRIVATE cpp third_party/sqlite)
target_compile_definitions(lmok_db_test PRIVATE LMOK_VERSION="${PROJECT_VERSION}")
target_link_libraries(lmok_db_test PRIVATE Threads::Threads)
if(UNIX)
  target_link_libraries(lmok_db_test PRIVATE m)
endif()
