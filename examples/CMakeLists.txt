add_executable(drain_lifecycle_demo lifecycle_demo.cpp)
target_link_libraries(drain_lifecycle_demo PRIVATE DrainFabric::drainfabric DrainFabric::Warnings)

add_executable(drain_correlated_demo correlated_demo.cpp)
target_link_libraries(drain_correlated_demo PRIVATE DrainFabric::drainfabric DrainFabric::Warnings)

add_executable(drain_inspect inspect.cpp)
target_link_libraries(drain_inspect PRIVATE DrainFabric::drainfabric DrainFabric::Warnings)
