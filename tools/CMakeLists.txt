add_executable(drainctl drainctl.cpp)
target_link_libraries(drainctl PRIVATE DrainFabric::drainfabric DrainFabric::Warnings)

add_executable(drainfabricd drainfabricd.cpp)
target_link_libraries(drainfabricd PRIVATE DrainFabric::drainfabric DrainFabric::Warnings)

add_executable(drainagent drainagent.cpp)
target_link_libraries(drainagent PRIVATE DrainFabric::drainfabric DrainFabric::Warnings)
