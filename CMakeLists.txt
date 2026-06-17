cmake_minimum_required(VERSION 3.16)
project(agent_mem_io LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(agentmem_core
  src/core/async_page_reader.cpp
  src/core/brute_force.cpp
  src/core/prefetch_planner.cpp
  src/core/pq_encoder.cpp
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
  src/graph/naive_disk_graph_index.cpp
  src/graph/vamana_builder.cpp
  src/storage/disk_index_reader.cpp
  src/storage/disk_index_writer.cpp
  src/storage/disk_record.cpp
  src/storage/lsm_tree.cpp
)

target_include_directories(agentmem_core PUBLIC include)

option(ENABLE_DIRECT_IO
  "Enable Linux O_DIRECT support" ON)

option(AGENTMEM_REQUIRE_LIBURING
  "Fail configuration when liburing is unavailable" OFF)

if (ENABLE_DIRECT_IO)
  target_compile_definitions(agentmem_core PUBLIC AGENTMEM_ENABLE_DIRECT_IO=1)
else()
  target_compile_definitions(agentmem_core PUBLIC AGENTMEM_ENABLE_DIRECT_IO=0)
endif()

find_path(LIBURING_INCLUDE_DIR liburing.h)
find_library(LIBURING_LIBRARY uring)

if (LIBURING_INCLUDE_DIR AND LIBURING_LIBRARY)
  target_compile_definitions(agentmem_core PUBLIC AGENTMEM_HAS_LIBURING=1)
  target_include_directories(agentmem_core PUBLIC ${LIBURING_INCLUDE_DIR})
  target_link_libraries(agentmem_core PUBLIC ${LIBURING_LIBRARY})
  message(STATUS "liburing enabled: ${LIBURING_LIBRARY}")
else()
  target_compile_definitions(agentmem_core PUBLIC AGENTMEM_HAS_LIBURING=0)
  if (AGENTMEM_REQUIRE_LIBURING)
    message(FATAL_ERROR
      "liburing requested but not found. Install liburing-dev or provide "
      "LIBURING_INCLUDE_DIR and LIBURING_LIBRARY.")
  endif()
  message(STATUS "liburing not found: io_uring backend will report fallback")
endif()

add_executable(agent_mem_io_tests
  tests/smoke_tests.cpp
)
target_link_libraries(agent_mem_io_tests PRIVATE agentmem_core)

add_executable(test_disk_record
  tests/test_disk_record.cpp
)
target_link_libraries(test_disk_record PRIVATE agentmem_core)

add_executable(test_disk_index_rw
  tests/test_disk_index_rw.cpp
)
target_link_libraries(test_disk_index_rw PRIVATE agentmem_core)

add_executable(test_memtable
  tests/dynamic/test_memtable.cpp
)
target_link_libraries(test_memtable PRIVATE agentmem_core)

add_executable(test_dynamic_wal
  tests/dynamic/test_wal.cpp
)
target_link_libraries(test_dynamic_wal PRIVATE agentmem_core)

add_executable(test_sstable
  tests/dynamic/test_sstable.cpp
)
target_link_libraries(test_sstable PRIVATE agentmem_core)

add_executable(test_compaction
  tests/dynamic/test_compaction.cpp
)
target_link_libraries(test_compaction PRIVATE agentmem_core)

add_executable(test_dynamic_insert
  tests/dynamic/test_dynamic_insert.cpp
)
target_link_libraries(test_dynamic_insert PRIVATE agentmem_core)

add_executable(bench_mixed_rw
  bench/bench_mixed_rw.cpp
)
target_link_libraries(bench_mixed_rw PRIVATE agentmem_core)

if (MSVC)
  target_compile_options(agentmem_core PRIVATE /W4)
  target_compile_options(agent_mem_io_tests PRIVATE /W4)
  target_compile_options(test_disk_record PRIVATE /W4)
  target_compile_options(test_disk_index_rw PRIVATE /W4)
  target_compile_options(test_memtable PRIVATE /W4)
  target_compile_options(test_dynamic_wal PRIVATE /W4)
  target_compile_options(test_sstable PRIVATE /W4)
  target_compile_options(test_compaction PRIVATE /W4)
  target_compile_options(test_dynamic_insert PRIVATE /W4)
  target_compile_options(bench_mixed_rw PRIVATE /W4)
else()
  target_compile_options(agentmem_core PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(agent_mem_io_tests PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(test_disk_record PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(test_disk_index_rw PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(test_memtable PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(test_dynamic_wal PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(test_sstable PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(test_compaction PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(test_dynamic_insert PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(bench_mixed_rw PRIVATE -Wall -Wextra -pedantic -O3)
endif()
