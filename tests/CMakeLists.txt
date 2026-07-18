# AeroEdge tests. Deterministic, exit-code-gated (like Quark samples): 0 = pass.
add_executable(walking_skeleton walking_skeleton.cpp)
target_link_libraries(walking_skeleton PRIVATE aero::core aero::nodes quark::quark)
add_test(NAME walking_skeleton COMMAND walking_skeleton)
set_tests_properties(walking_skeleton PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Invariant gate: the flow-execute path is 0-alloc on the steady path (I3). Not a benchmark.
add_executable(flow_zero_alloc flow_zero_alloc.cpp)
target_link_libraries(flow_zero_alloc PRIVATE aero::core aero::nodes)
add_test(NAME flow_zero_alloc COMMAND flow_zero_alloc)
set_tests_properties(flow_zero_alloc PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-2 ingestion gate: a driver streams N frames through the Quark 024 StreamChannel into an
# actor's flow — cross-thread SPSC — asserting FIFO, no loss, no dup, and payload integrity (006 §2).
add_executable(stream_ingest stream_ingest.cpp)
target_link_libraries(stream_ingest PRIVATE aero::core aero::nodes aero::drivers quark::quark)
add_test(NAME stream_ingest COMMAND stream_ingest)
set_tests_properties(stream_ingest PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-2 backpressure gate (the load-bearing one, 006 §3 "backpressure, not shedding"): a fast
# producer against a small ring + slow drain — 0 dropped frames, bounded occupancy, stalls observed.
add_executable(stream_backpressure stream_backpressure.cpp)
target_link_libraries(stream_backpressure PRIVATE aero::core aero::nodes aero::drivers quark::quark)
add_test(NAME stream_backpressure COMMAND stream_backpressure)
set_tests_properties(stream_backpressure PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
