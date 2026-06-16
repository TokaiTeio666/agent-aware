cmake_minimum_required(VERSION 3.16)
project(agent_mem_io LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(agentmem_core
  src/benchmark/metrics.cpp
  src/core/brute_force.cpp
  src/core/pq_encoder.cpp
  src/data/dataset.cpp
  src/engine/storage_engine.cpp
  src/graph/disk_graph_index.cpp
  src/graph/vamana_builder.cpp
  src/storage/lsm_tree.cpp
)

target_include_directories(agentmem_core PUBLIC include)

option(AGENTMEM_REQUIRE_LIBURING
  "Fail configuration when liburing is unavailable" OFF)

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

add_executable(agent_mem_io_benchmark
  src/app/agent_mem_io_cli.cpp
  src/benchmark/cli_args.cpp
  src/benchmark/benchmark_runner.cpp
)
target_link_libraries(agent_mem_io_benchmark PRIVATE agentmem_core)
set_target_properties(agent_mem_io_benchmark PROPERTIES
  OUTPUT_NAME agentmem_flow)

add_executable(agent_mem_io_tests
  tests/smoke_tests.cpp
)
target_link_libraries(agent_mem_io_tests PRIVATE agentmem_core)

if (MSVC)
  target_compile_options(agentmem_core PRIVATE /W4)
  target_compile_options(agent_mem_io_benchmark PRIVATE /W4)
  target_compile_options(agent_mem_io_tests PRIVATE /W4)
else()
  target_compile_options(agentmem_core PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(agent_mem_io_benchmark PRIVATE -Wall -Wextra -pedantic -O3)
  target_compile_options(agent_mem_io_tests PRIVATE -Wall -Wextra -pedantic -O3)
endif()
