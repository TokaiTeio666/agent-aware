cmake_minimum_required(VERSION 3.16)

project(agent_aware VERSION 0.1.0 LANGUAGES CXX)

option(ENABLE_DIRECT_IO "Enable Linux O_DIRECT support" ON)
option(AGENT_AWARE_REQUIRE_LIBURING "Fail configure when liburing is unavailable" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  find_path(AGENT_AWARE_LIBURING_INCLUDE_DIR liburing.h)
  find_library(AGENT_AWARE_LIBURING_LIBRARY NAMES uring)
endif()

set(AGENT_AWARE_HAS_LIBURING OFF)
if(AGENT_AWARE_LIBURING_INCLUDE_DIR AND AGENT_AWARE_LIBURING_LIBRARY)
  set(AGENT_AWARE_HAS_LIBURING ON)
endif()

if(AGENT_AWARE_REQUIRE_LIBURING AND NOT AGENT_AWARE_HAS_LIBURING)
  message(FATAL_ERROR "AGENT_AWARE_REQUIRE_LIBURING=ON but liburing was not found")
endif()

if(AGENT_AWARE_HAS_LIBURING)
  message(STATUS "liburing found: ${AGENT_AWARE_LIBURING_LIBRARY}")
else()
  message(STATUS "liburing not found; io_uring mode will fall back to pread")
endif()

set(AGENT_AWARE_LIBRARY_SOURCES
  src/core/async_page_reader.cpp
  src/core/brute_force.cpp
  src/core/pq_encoder.cpp
  src/core/prefetch_planner.cpp
  src/core/query_page_session.cpp
  src/data/dataset.cpp
  src/dynamic/compaction.cpp
  src/dynamic/dynamic_write_manager.cpp
  src/dynamic/manifest.cpp
  src/dynamic/memtable.cpp
  src/dynamic/sstable.cpp
  src/dynamic/wal.cpp
  src/engine/storage_engine.cpp
  src/graph/disk_graph_builder.cpp
  src/graph/disk_graph_index.cpp
  src/graph/disk_page_codec.cpp
  src/graph/entry_selector.cpp
  src/graph/vamana_builder.cpp
)

add_library(agent_aware STATIC ${AGENT_AWARE_LIBRARY_SOURCES})
add_library(agent_aware::agent_aware ALIAS agent_aware)

target_include_directories(agent_aware
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(agent_aware PUBLIC Threads::Threads)

if(ENABLE_DIRECT_IO)
  target_compile_definitions(agent_aware PUBLIC AGENT_AWARE_ENABLE_DIRECT_IO=1)
else()
  target_compile_definitions(agent_aware PUBLIC AGENT_AWARE_ENABLE_DIRECT_IO=0)
endif()

if(AGENT_AWARE_HAS_LIBURING)
  target_include_directories(agent_aware PRIVATE "${AGENT_AWARE_LIBURING_INCLUDE_DIR}")
  target_link_libraries(agent_aware PRIVATE "${AGENT_AWARE_LIBURING_LIBRARY}")
  target_compile_definitions(agent_aware PRIVATE AGENT_AWARE_HAS_LIBURING=1)
else()
  target_compile_definitions(agent_aware PRIVATE AGENT_AWARE_HAS_LIBURING=0)
endif()

add_executable(agent-aware src/sift_search_benchmark.cpp)
target_link_libraries(agent-aware PRIVATE agent_aware::agent_aware)

add_executable(bench_mixed_rw src/mixed_rw_benchmark.cpp)
target_link_libraries(bench_mixed_rw PRIVATE agent_aware::agent_aware)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(
    NAME contract_smoke
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract_smoke.py"
            "$<TARGET_FILE:agent-aware>"
            "$<TARGET_FILE:bench_mixed_rw>"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/agent_aware.py"
  )
endif()
