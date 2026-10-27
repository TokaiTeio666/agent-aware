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
  src/core/platform_io.cpp
  src/core/pq_encoder.cpp
  src/core/prefetch_adaptive_controller.cpp
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

add_executable(agent-aware-search src/sift_search_benchmark.cpp)
target_link_libraries(agent-aware-search PRIVATE agent_aware::agent_aware)

add_executable(agent-aware-build src/index_builder.cpp)
target_link_libraries(agent-aware-build PRIVATE agent_aware::agent_aware)

add_executable(bench_mixed_rw src/mixed_rw_benchmark.cpp)
target_link_libraries(bench_mixed_rw PRIVATE agent_aware::agent_aware)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_executable(dynamic_write_concurrency_test
                 tests/dynamic_write_concurrency.cpp)
  target_link_libraries(dynamic_write_concurrency_test
                        PRIVATE agent_aware::agent_aware)
  add_test(NAME dynamic_write_concurrency
           COMMAND dynamic_write_concurrency_test)
  add_executable(packed_layout_v3_test tests/packed_layout_v3.cpp)
  target_link_libraries(packed_layout_v3_test PRIVATE agent_aware::agent_aware)
  add_test(NAME packed_layout_v3 COMMAND packed_layout_v3_test)
  add_executable(async_page_reader_test
                 tests/async_page_reader.cpp)
  target_link_libraries(async_page_reader_test
                        PRIVATE agent_aware::agent_aware)
  add_test(NAME async_page_reader COMMAND async_page_reader_test)
  # 扩展路径的 demand 读必须成批提交，且批量不得改变 top-k 与页数。
  add_executable(demand_io_batching_test tests/demand_io_batching.cpp)
  target_link_libraries(demand_io_batching_test
                        PRIVATE agent_aware::agent_aware)
  add_test(NAME demand_io_batching COMMAND demand_io_batching_test)
  # 召回结论的诊断工具，需要数据集/索引路径，手动运行，不进 ctest：
  #   brute_force_check  精确暴力基线（真值文件是否可信）
  #   index_integrity    索引向量逐位比对 + 入口可达率 + 邻居局部性
  #   graph_probe        入口/真值起点/多起点三组召回对照
  add_executable(brute_force_check tests/brute_force_check.cpp)
  target_link_libraries(brute_force_check PRIVATE agent_aware::agent_aware)
  add_executable(graph_probe tests/graph_probe.cpp)
  target_link_libraries(graph_probe PRIVATE agent_aware::agent_aware)
  add_executable(index_integrity tests/index_integrity.cpp)
  target_link_libraries(index_integrity PRIVATE agent_aware::agent_aware)
  #   entry_audit        入口节点入度审计（入口是否是个"只出不进"的起点）
  add_executable(entry_audit tests/entry_audit.cpp)
  target_link_libraries(entry_audit PRIVATE agent_aware::agent_aware)
  add_executable(packed_degree_guard_test tests/packed_degree_guard.cpp)
  target_link_libraries(packed_degree_guard_test
                        PRIVATE agent_aware::agent_aware)
  add_test(NAME packed_degree_guard COMMAND packed_degree_guard_test)
  add_executable(u_reach tests/u_reach.cpp)
  target_link_libraries(u_reach PRIVATE agent_aware::agent_aware)
  add_executable(node_neighborhood tests/node_neighborhood.cpp)
  target_link_libraries(node_neighborhood PRIVATE agent_aware::agent_aware)
  add_executable(neighbor_recall tests/neighbor_recall.cpp)
  target_link_libraries(neighbor_recall PRIVATE agent_aware::agent_aware)
  add_executable(platform_io_test tests/platform_io.cpp)
  target_link_libraries(platform_io_test PRIVATE agent_aware::agent_aware)
  add_test(NAME platform_io COMMAND platform_io_test)
  add_executable(prefetch_adaptive_controller_test
                 tests/prefetch_adaptive_controller.cpp)
  target_link_libraries(prefetch_adaptive_controller_test
                        PRIVATE agent_aware::agent_aware)
  add_test(NAME prefetch_adaptive_controller
           COMMAND prefetch_adaptive_controller_test)
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
