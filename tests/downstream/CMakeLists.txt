cmake_minimum_required(VERSION 3.20)

# Independent downstream consumer.
#
# This project is deliberately outside the Drain Fabric source tree. It proves
# that the installed package can be found, linked, and driven by a separate
# CMake project that only ever sees the installed headers and library.

project(DrainFabricDownstream
  VERSION 1.0.0
  DESCRIPTION "Independent consumer of the installed DrainFabric package"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(DrainFabric 1.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE DrainFabric::drainfabric)

if(MSVC)
  target_compile_options(downstream_consumer PRIVATE /W4 /WX /permissive-)
else()
  target_compile_options(downstream_consumer PRIVATE -Wall -Wextra -Werror)
endif()

add_test(NAME downstream_consumer COMMAND downstream_consumer)
enable_testing()
