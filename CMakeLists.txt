cmake_minimum_required(VERSION 3.16)
project(agent_aware LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(AGENT_AWARE_CORE_SOURCES
  src/agent_aware/core/async_page_reader.cpp
  src/agent_aware/core/brute_force.cpp
  src/agent_aware/core/prefetch_planner.cpp
  src/agent_aware/core/pq_encoder.cpp
  src/agent_aware/core/query_page_session.cpp
  src/agent_aware/data/dataset.cpp
  src/agent_aware/dynamic/compaction.cpp
  src/agent_aware/dynamic/dynamic_write_manager.cpp
  src/agent_aware/dynamic/manifest.cpp
  src/agent_aware/dynamic/memtable.cpp
  src/agent_aware/dynamic/sstable.cpp
  src/agent_aware/dynamic/wal.cpp
  src/agent_aware/engine/storage_engine.cpp
  src/agent_aware/graph/disk_graph_builder.cpp
  src/agent_aware/graph/disk_graph_index.cpp
  src/agent_aware/graph/disk_page_codec.cpp
  src/agent_aware/graph/entry_selector.cpp
  src/agent_aware/graph/naive_disk_graph_index.cpp
  src/agent_aware/graph/vamana_builder.cpp
  src/agent_aware/storage/disk_index_reader.cpp
  src/agent_aware/storage/disk_index_writer.cpp
  src/agent_aware/storage/disk_record.cpp
  src/agent_aware/storage/lsm_tree.cpp
)

add_library(agent_aware_core ${AGENT_AWARE_CORE_SOURCES})
target_include_directories(agent_aware_core PUBLIC include)

option(ENABLE_DIRECT_IO
  "Enable Linux O_DIRECT support" ON)

option(AGENT_AWARE_REQUIRE_LIBURING
  "Fail configuration when liburing is unavailable" OFF)

if (ENABLE_DIRECT_IO)
  target_compile_definitions(agent_aware_core PUBLIC AGENT_AWARE_ENABLE_DIRECT_IO=1)
else()
  target_compile_definitions(agent_aware_core PUBLIC AGENT_AWARE_ENABLE_DIRECT_IO=0)
endif()

find_path(LIBURING_INCLUDE_DIR liburing.h)
find_library(LIBURING_LIBRARY uring)

if (LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
  target_compile_definitions(agent_aware_core PUBLIC AGENT_AWARE_HAS_LIBURING=1)
  target_include_directories(agent_aware_core PUBLIC ${LIBURING_INCLUDE_DIR})
  target_link_libraries(agent_aware_core PUBLIC ${LIBURING_LIBRARY})
  message(STATUS "liburing enabled: ${LIBURING_LIBRARY}")
else()
  target_compile_definitions(agent_aware_core PUBLIC AGENT_AWARE_HAS_LIBURING=0)
  if (AGENT_AWARE_REQUIRE_LIBURING)
    message(FATAL_ERROR
      "liburing requested but not found. Install liburing-dev or provide "
      "LIBURING_INCLUDE_DIR and LIBURING_LIBRARY.")
  endif()
  message(STATUS "liburing not found: io_uring backend will report fallback")
endif()

function(agent_aware_add_test target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE agent_aware_core)
  list(APPEND AGENT_AWARE_TARGETS ${target})
  set(AGENT_AWARE_TARGETS "${AGENT_AWARE_TARGETS}" PARENT_SCOPE)
endfunction()

function(agent_aware_add_benchmark target source)
  add_executable(${target} ${source})
  target_link_libraries(${target} PRIVATE agent_aware_core)
  list(APPEND AGENT_AWARE_TARGETS ${target})
  set(AGENT_AWARE_TARGETS "${AGENT_AWARE_TARGETS}" PARENT_SCOPE)
endfunction()

set(AGENT_AWARE_TARGETS agent_aware_core)

agent_aware_add_test(agent_aware_tests
  tests/unit/core/test_core_smoke.cpp)
agent_aware_add_test(test_disk_record
  tests/unit/storage/test_disk_record.cpp)
agent_aware_add_test(test_disk_index_rw
  tests/unit/storage/test_disk_index_rw.cpp)
agent_aware_add_test(test_memtable
  tests/unit/dynamic/test_memtable.cpp)
agent_aware_add_test(test_dynamic_wal
  tests/unit/dynamic/test_wal.cpp)
agent_aware_add_test(test_sstable
  tests/unit/dynamic/test_sstable.cpp)
agent_aware_add_test(test_compaction
  tests/unit/dynamic/test_compaction.cpp)
agent_aware_add_test(test_dynamic_insert
  tests/unit/dynamic/test_dynamic_insert.cpp)
agent_aware_add_test(test_entry_selector
  tests/unit/graph/test_entry_selector.cpp)

agent_aware_add_benchmark(bench_mixed_rw
  tools/benchmarks/mixed_rw_benchmark.cpp)
agent_aware_add_benchmark(agent_aware_flow
  tools/benchmarks/sift_search_benchmark.cpp)

if (MSVC)
  set(AGENT_AWARE_WARNING_FLAGS /W4)
else()
  set(AGENT_AWARE_WARNING_FLAGS -Wall -Wextra -pedantic -O3)
endif()

foreach(target IN LISTS AGENT_AWARE_TARGETS)
  target_compile_options(${target} PRIVATE ${AGENT_AWARE_WARNING_FLAGS})
endforeach()
