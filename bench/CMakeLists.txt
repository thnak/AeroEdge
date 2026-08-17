# AeroEdge benchmarks — NOT ctest gates (no pass/fail, prints numbers for a human to read). Built only
# when -DAEROEDGE_BUILD_BENCH=ON (see root CMakeLists.txt), mirroring QUARK_BUILD_BENCH's default-OFF
# posture for the same reason: keep normal build/test runs lean.
add_executable(broker_bench broker/broker_bench.cpp)
target_link_libraries(broker_bench PRIVATE aero::broker Threads::Threads)
