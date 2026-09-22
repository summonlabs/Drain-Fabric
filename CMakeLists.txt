cmake_minimum_required(VERSION 3.20)

project(DrainFabric
  VERSION 1.0.0
  DESCRIPTION "Vendor-neutral Summon Software Labs Fabric OS runtime for controlled resource drain"
  HOMEPAGE_URL "https://github.com/summonlabs/Drain-Fabric"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(DRAINFABRIC_BUILD_TESTS "Build the Drain Fabric test suite" ON)
option(DRAINFABRIC_BUILD_TOOLS "Build drainctl / drainfabricd" ON)
option(DRAINFABRIC_BUILD_EXAMPLES "Build Drain Fabric examples" ON)
option(DRAINFABRIC_BUILD_BENCHMARKS "Build Drain Fabric benchmarks" OFF)
option(DRAINFABRIC_WERROR "Treat first-party compiler warnings as errors" ON)
option(DRAINFABRIC_SANITIZE "Enable AddressSanitizer + UndefinedBehaviorSanitizer where supported" OFF)
option(DRAINFABRIC_SHARED "Build drainfabric as a shared library" OFF)

if(DRAINFABRIC_SHARED)
  set(BUILD_SHARED_LIBS ON)
endif()

set(DRAINFABRIC_WARNINGS "")
if(MSVC)
  list(APPEND DRAINFABRIC_WARNINGS /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor)
  if(DRAINFABRIC_WERROR)
    list(APPEND DRAINFABRIC_WARNINGS /WX)
  endif()
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  list(APPEND DRAINFABRIC_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual)
  if(DRAINFABRIC_WERROR)
    list(APPEND DRAINFABRIC_WARNINGS -Werror)
  endif()
endif()

if(DRAINFABRIC_SANITIZE)
  if(MSVC)
    add_compile_options(/fsanitize=address)
    add_link_options(/fsanitize=address)
    # The MSVC address sanitizer links against a dynamic runtime that lives next
    # to the compiler. Record its directory so the test environment can find it
    # regardless of the generator (Visual Studio adds it automatically; Ninja
    # and Makefiles do not).
    get_filename_component(DRAINFABRIC_ASAN_RUNTIME_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(DRAINFABRIC_ASAN_RUNTIME_DIR "${DRAINFABRIC_ASAN_RUNTIME_DIR}" CACHE INTERNAL
        "Directory holding the MSVC address sanitizer runtime")
  else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

# First-party warning policy. Applied directly to the library so that the
# exported package depends only on real, installable targets, and exposed as an
# interface target for the tests, tools, examples, and benchmarks.
add_library(drainfabric_warnings INTERFACE)
target_compile_options(drainfabric_warnings INTERFACE ${DRAINFABRIC_WARNINGS})
add_library(DrainFabric::Warnings ALIAS drainfabric_warnings)

find_package(Threads REQUIRED)

add_subdirectory(src)

if(DRAINFABRIC_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(DRAINFABRIC_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(DRAINFABRIC_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(DRAINFABRIC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install / export
# ---------------------------------------------------------------------------
install(TARGETS drainfabric
  EXPORT DrainFabricTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(DRAINFABRIC_BUILD_TOOLS)
  install(TARGETS drainctl drainfabricd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/DrainFabricConfigVersion.cmake"
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
  ARCH_INDEPENDENT)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DrainFabricConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/DrainFabricConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DrainFabric)

install(EXPORT DrainFabricTargets
  FILE DrainFabricTargets.cmake
  NAMESPACE DrainFabric::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DrainFabric)

install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/DrainFabricConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/DrainFabricConfigVersion.cmake"
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/DrainFabric)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
  DESTINATION ${CMAKE_INSTALL_DATADIR}/DrainFabric)
