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

# Phase-4 schema gate (009 §2, 013 T3): parse a well-formed Application → assert fields; malformed /
# incomplete JSON → a clean error value (no throw), so the API can 4xx instead of aborting.
add_executable(schema_load schema_load.cpp)
target_link_libraries(schema_load PRIVATE aero::schema)
add_test(NAME schema_load COMMAND schema_load)
set_tests_properties(schema_load PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-4 LOAD-BEARING gate (009): in-process, NO socket — deploy a declarative Application into a
# Runtime and assert the compiled flow runs every ingested frame (declarative deploy → running flow).
add_executable(runtime_deploy runtime_deploy.cpp)
target_link_libraries(runtime_deploy PRIVATE aero::runtime)
add_test(NAME runtime_deploy COMMAND runtime_deploy)
set_tests_properties(runtime_deploy PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-4 API gate (013 §5/§9): start the REST server on an ephemeral port, POST an Application + GET
# /status over a real socket, assert it is reported, then shut down cleanly. Wait-for-ready by polling.
add_executable(api_integration api_integration.cpp)
target_link_libraries(api_integration PRIVATE aero::api)
add_test(NAME api_integration COMMAND api_integration)
set_tests_properties(api_integration PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-5 validation gate (009 §3 P1, 004 §2.1): the Flow Compiler validates at deploy — invalid
# Applications (unknown type_id, empty flow, no source/output, bad config) are rejected as values
# (no crash, no half-deploy); a valid one is accepted.
add_executable(flow_validation flow_validation.cpp)
target_link_libraries(flow_validation PRIVATE aero::runtime)
add_test(NAME flow_validation COMMAND flow_validation)
set_tests_properties(flow_validation PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-5 LOAD-BEARING gate (009 §4 P2): hot-reload a live actor's flow while Commands flow — 0
# dropped, 0 duplicated (exactly-once), the pre-reload frames run the old flow and post-reload the
# new one, proven by an exact frame count + output-sum split. Mailbox-ordered swap on a Sequential
# actor (Quark 001/ADR-002 FIFO). Must be TSan-clean.
add_executable(hot_reload hot_reload.cpp)
target_link_libraries(hot_reload PRIVATE aero::runtime)
add_test(NAME hot_reload COMMAND hot_reload)
set_tests_properties(hot_reload PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-5 classification gate (009 §4 P3): a Live change (node config) hot-reloads; a BuildOnly change
# (actor key, persistence mode) is rejected with a clear error and never half-applied.
add_executable(reload_buildonly reload_buildonly.cpp)
target_link_libraries(reload_buildonly PRIVATE aero::runtime)
add_test(NAME reload_buildonly COMMAND reload_buildonly)
set_tests_properties(reload_buildonly PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-5 versioning gate (009 §6): reload A->B then rollback returns behavior to A.
add_executable(rollback rollback.cpp)
target_link_libraries(rollback PRIVATE aero::runtime)
add_test(NAME rollback COMMAND rollback)
set_tests_properties(rollback PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-5 API gate (009 §4/§6, 013 §5): PUT /apps/{name} (reload) + POST /apps/{name}/rollback over a
# real socket; a BuildOnly reload is a clean 400. Ephemeral port, health-poll, clean shutdown.
add_executable(api_reload api_reload.cpp)
target_link_libraries(api_reload PRIVATE aero::api)
add_test(NAME api_reload COMMAND api_reload)
set_tests_properties(api_reload PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
