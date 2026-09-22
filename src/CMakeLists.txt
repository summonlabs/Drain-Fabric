set(DRAINFABRIC_SOURCES
  identity.cpp
  version.cpp
  result.cpp
  clock.cpp
  crc32c.cpp
  digest.cpp
  json.cpp
  obligation.cpp
  topology.cpp
  policy.cpp
  lifecycle.cpp
  evidence.cpp
  authority.cpp
  admission.cpp
  decision.cpp
  drain_set.cpp
  explain.cpp
  engine.cpp
  persistence.cpp
  runtime.cpp
  wire.cpp
  socket.cpp
  server.cpp
  agent.cpp
)

add_library(drainfabric ${DRAINFABRIC_SOURCES})
add_library(DrainFabric::drainfabric ALIAS drainfabric)

target_include_directories(drainfabric
  PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../src")

target_link_libraries(drainfabric PUBLIC Threads::Threads)
target_compile_options(drainfabric PRIVATE ${DRAINFABRIC_WARNINGS})

set_target_properties(drainfabric PROPERTIES
  OUTPUT_NAME drainfabric
  POSITION_INDEPENDENT_CODE ON
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR})

if(WIN32)
  target_link_libraries(drainfabric PRIVATE ws2_32)
endif()

if(DRAINFABRIC_SHARED)
  target_compile_definitions(drainfabric PUBLIC DRAINFABRIC_SHARED)
endif()
