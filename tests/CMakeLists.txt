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

# Phase-3 load-bearing gate (007 §2/§6/§7): durable actor state survives a simulated restart — a
# fresh actor recovers its snapshot from the SAME Store (InMemoryStore + crash-durable FileStore),
# while a stateful node's transient window rebuilds cold ("nodes compute, actors remember").
add_executable(persistence_recovery persistence_recovery.cpp)
target_link_libraries(persistence_recovery PRIVATE aero::core aero::nodes quark::quark)
add_test(NAME persistence_recovery COMMAND persistence_recovery)
set_tests_properties(persistence_recovery PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-3 stateful-node gate (007 §6): engine-free proof that transient node state is lost + rebuilt
# cold on restart while promoted (durable) actor state survives (S6).
add_executable(stateful_node stateful_node.cpp)
target_link_libraries(stateful_node PRIVATE aero::core aero::nodes)
add_test(NAME stateful_node COMMAND stateful_node)
set_tests_properties(stateful_node PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
