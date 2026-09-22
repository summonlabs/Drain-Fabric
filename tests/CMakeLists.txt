add_library(drain_test_support STATIC
  support/test.cpp
  support/process.cpp)
add_library(DrainFabric::TestSupport ALIAS drain_test_support)

target_include_directories(drain_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(drain_test_support PUBLIC DrainFabric::drainfabric PRIVATE DrainFabric::Warnings)

set(DRAINFABRIC_TEST_BINARIES
  drain_unit_tests
  drain_property_tests
  drain_adversarial_tests
  drain_concurrency_tests
  drain_restart_tests
  drain_transport_tests
  drain_e2e_tests)

foreach(test_target IN LISTS DRAINFABRIC_TEST_BINARIES)
  add_executable(${test_target} ${test_target}.cpp)
  target_link_libraries(${test_target} PRIVATE DrainFabric::TestSupport DrainFabric::Warnings)
endforeach()

# The transport suite drives real child processes, so it needs the
# command-line binaries next to it.
target_compile_definitions(drain_transport_tests PRIVATE
  DRAINFABRIC_TOOLS_DIR="$<TARGET_FILE_DIR:drainfabricd>")

foreach(test_target IN LISTS DRAINFABRIC_TEST_BINARIES)
  add_test(NAME ${test_target} COMMAND ${test_target})
  set_tests_properties(${test_target} PROPERTIES LABELS "drainfabric")
  if(DRAINFABRIC_SANITIZE AND MSVC AND DRAINFABRIC_ASAN_RUNTIME_DIR)
    set_tests_properties(${test_target} PROPERTIES
      ENVIRONMENT "PATH=${DRAINFABRIC_ASAN_RUNTIME_DIR};$ENV{PATH}")
  endif()
endforeach()
