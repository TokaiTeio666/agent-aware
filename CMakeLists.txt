cmake_minimum_required(VERSION 3.16)

project(agent_aware VERSION 0.1.0 LANGUAGES CXX)

option(ENABLE_DIRECT_IO "Enable Linux O_DIRECT support" ON)
option(AGENT_AWARE_REQUIRE_LIBURING "Fail configure when liburing is unavailable" OFF)
option(AGENT_AWARE_BUILD_NAPI "Build the agentAwareNative Node-API module" OFF)
set(AGENT_AWARE_NAPI_INCLUDE_DIR "" CACHE PATH "Directory containing node_api.h")
set(AGENT_AWARE_NAPI_LIBRARY "" CACHE FILEPATH "Optional HarmonyOS N-API library")

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
  src/core/memory_budget.cpp
  src/core/pq_encoder.cpp
  src/core/prefetch_adaptive_controller.cpp
  src/core/query_page_session.cpp
  src/core/reader_context.cpp
  src/data/dataset.cpp
  src/dynamic/compaction.cpp
  src/dynamic/dynamic_write_manager.cpp
  src/dynamic/manifest.cpp
  src/dynamic/memtable.cpp
  src/dynamic/sstable.cpp
  src/dynamic/wal.cpp
  src/engine/storage_engine.cpp
  src/graph/disk_graph_builder.cpp
  src/graph/packed_graph_generation.cpp
  src/graph/disk_graph_index.cpp
  src/graph/disk_page_codec.cpp
  src/graph/vamana_builder.cpp
  src/harmony/harmony_engine_facade.cpp
  src/harmony/operation_scheduler.cpp
  src/platform/page_io_backend.cpp
)

add_library(agent_aware STATIC ${AGENT_AWARE_LIBRARY_SOURCES})
set_target_properties(agent_aware PROPERTIES POSITION_INDEPENDENT_CODE ON)
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

add_executable(agent-aware-search src/sift_search_benchmark.cpp)
target_link_libraries(agent-aware-search PRIVATE agent_aware::agent_aware)

add_executable(agent-aware-build src/index_builder.cpp)
target_link_libraries(agent-aware-build PRIVATE agent_aware::agent_aware)

add_executable(bench_mixed_rw src/mixed_rw_benchmark.cpp)
target_link_libraries(bench_mixed_rw PRIVATE agent_aware::agent_aware)

if(AGENT_AWARE_BUILD_NAPI)
  if(NOT AGENT_AWARE_NAPI_INCLUDE_DIR)
    message(FATAL_ERROR "AGENT_AWARE_BUILD_NAPI requires AGENT_AWARE_NAPI_INCLUDE_DIR")
  endif()
  add_library(agentAwareNative SHARED src/harmony/napi_module.cpp)
  target_include_directories(agentAwareNative PRIVATE "${AGENT_AWARE_NAPI_INCLUDE_DIR}")
  target_link_libraries(agentAwareNative PRIVATE agent_aware::agent_aware)
  if(AGENT_AWARE_NAPI_LIBRARY)
    target_link_libraries(agentAwareNative PRIVATE "${AGENT_AWARE_NAPI_LIBRARY}")
  endif()
  target_compile_definitions(agentAwareNative PRIVATE NAPI_VERSION=8)
  set_target_properties(agentAwareNative PROPERTIES OUTPUT_NAME agentAwareNative)
endif()

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(dynamic_write_concurrency_test
                 tests/dynamic_write_concurrency.cpp)
  target_link_libraries(dynamic_write_concurrency_test
                        PRIVATE agent_aware::agent_aware)
  add_test(NAME dynamic_write_concurrency
           COMMAND dynamic_write_concurrency_test)
  add_executable(compact_memtable_test tests/compact_memtable.cpp)
  target_link_libraries(compact_memtable_test PRIVATE agent_aware::agent_aware)
  add_test(NAME compact_memtable COMMAND compact_memtable_test)
  add_executable(packed_layout_v3_test tests/packed_layout_v3.cpp)
  target_link_libraries(packed_layout_v3_test PRIVATE agent_aware::agent_aware)
  add_test(NAME packed_layout_v3 COMMAND packed_layout_v3_test)
  add_executable(top_l_candidate_pool_test tests/top_l_candidate_pool.cpp)
  target_link_libraries(top_l_candidate_pool_test PRIVATE agent_aware::agent_aware)
  add_test(NAME top_l_candidate_pool COMMAND top_l_candidate_pool_test)
  add_executable(packed_generation_test tests/packed_generation.cpp)
  target_link_libraries(packed_generation_test PRIVATE agent_aware::agent_aware)
  add_test(NAME packed_generation COMMAND packed_generation_test)
  add_executable(online_generation_merge_test tests/online_generation_merge.cpp)
  target_link_libraries(online_generation_merge_test PRIVATE agent_aware::agent_aware)
  add_test(NAME online_generation_merge COMMAND online_generation_merge_test)
  add_executable(async_page_reader_test
                 tests/async_page_reader.cpp)
  target_link_libraries(async_page_reader_test
                        PRIVATE agent_aware::agent_aware)
  add_test(NAME async_page_reader COMMAND async_page_reader_test)
  add_executable(prefetch_adaptive_controller_test
                 tests/prefetch_adaptive_controller.cpp)
  target_link_libraries(prefetch_adaptive_controller_test
                        PRIVATE agent_aware::agent_aware)
  add_test(NAME prefetch_adaptive_controller
           COMMAND prefetch_adaptive_controller_test)
  add_executable(harmony_facade_test tests/harmony_facade.cpp)
  target_link_libraries(harmony_facade_test PRIVATE agent_aware::agent_aware)
  add_test(NAME harmony_facade COMMAND harmony_facade_test)
  add_executable(page_io_backend_test tests/page_io_backend.cpp)
  target_link_libraries(page_io_backend_test PRIVATE agent_aware::agent_aware)
  add_test(NAME page_io_backend COMMAND page_io_backend_test)
  add_executable(operation_scheduler_test tests/operation_scheduler.cpp)
  target_link_libraries(operation_scheduler_test PRIVATE agent_aware::agent_aware)
  add_test(NAME operation_scheduler COMMAND operation_scheduler_test)
  add_test(
    NAME contract_smoke
    COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract_smoke.py"
            "$<TARGET_FILE:agent-aware-build>"
            "$<TARGET_FILE:agent-aware-search>"
            "$<TARGET_FILE:bench_mixed_rw>"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/agent_aware.py"
  )
endif()
