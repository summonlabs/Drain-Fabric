add_executable(drain_benchmark benchmark.cpp)
target_link_libraries(drain_benchmark PRIVATE DrainFabric::drainfabric DrainFabric::Warnings)
