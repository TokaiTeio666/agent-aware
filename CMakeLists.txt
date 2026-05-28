cmake_minimum_required(VERSION 3.16)
project(agentmem_flow LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(agentmem_flow
  src/main.cpp
  src/dataset.cpp
  src/brute_force.cpp
  src/dynamic_index.cpp
  src/graph_index.cpp
  src/metrics.cpp
)

target_include_directories(agentmem_flow PRIVATE include)

if (MSVC)
  target_compile_options(agentmem_flow PRIVATE /W4)
else()
  target_compile_options(agentmem_flow PRIVATE -Wall -Wextra -pedantic -O3)
endif()
