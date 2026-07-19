# AeroEdge tests. Deterministic, exit-code-gated (like Quark samples): 0 = pass.
add_executable(walking_skeleton walking_skeleton.cpp)
target_link_libraries(walking_skeleton PRIVATE aero::core aero::nodes quark::quark)
add_test(NAME walking_skeleton COMMAND walking_skeleton)
set_tests_properties(walking_skeleton PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Invariant gate: the flow-execute path is 0-alloc on the steady path (I3). Not a benchmark.
# It replaces global operator new/delete to count allocations — which collides with clang's TSan C++
# runtime (libclang_rt.tsan_cxx also defines them → "multiple definition" link error). The gate is a
# deterministic single-thread allocation count with no concurrency to check, so it does not belong in
# the thread-sanitizer build; it still runs in the Release + ASan cells on both compilers.
if(NOT AEROEDGE_SANITIZER MATCHES "thread")
  add_executable(flow_zero_alloc flow_zero_alloc.cpp)
  target_link_libraries(flow_zero_alloc PRIVATE aero::core aero::nodes)
  add_test(NAME flow_zero_alloc COMMAND flow_zero_alloc)
  set_tests_properties(flow_zero_alloc PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
endif()

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

# ---- Phase-6: extension model (008) ----------------------------------------------------------------
# Sample NATIVE extensions built as CMake MODULE libraries (`.so`). Being normal targets in this tree
# they inherit add_compile_options / add_link_options — so under a sanitizer build they are compiled
# AND linked with the SAME -fsanitize flags as the host, keeping dlopen clean under ASan/TSan. They
# compile against aero-sdk-native ONLY (the C ABI) — no aero-core, no STL type crosses the boundary (E2).
add_library(aero_ext_gain MODULE "${CMAKE_CURRENT_SOURCE_DIR}/../examples/ext_native/gain_node.cpp")
target_link_libraries(aero_ext_gain PRIVATE aero::sdk-native)
set_target_properties(aero_ext_gain PROPERTIES PREFIX "" OUTPUT_NAME "aero_ext_gain")

add_library(aero_ext_badabi MODULE "${CMAKE_CURRENT_SOURCE_DIR}/../examples/ext_native/gain_node_badabi.cpp")
target_link_libraries(aero_ext_badabi PRIVATE aero::sdk-native)
set_target_properties(aero_ext_badabi PROPERTIES PREFIX "" OUTPUT_NAME "aero_ext_badabi")

# Phase-6 LOAD-BEARING gate (008 §2, E1/E2/E6): dlopen the sample `.so`, ABI-check it, register its
# aero.ext.gain node, deploy a flow that uses it, drive frames, and assert the NATIVE node transformed
# the data; also assert a wrong-ABI `.so` is rejected cleanly. The `.so` paths are injected here.
add_executable(native_extension native_extension.cpp)
target_link_libraries(native_extension PRIVATE aero::runtime aero::ext)
target_compile_definitions(native_extension PRIVATE
  AERO_EXT_GAIN_SO="$<TARGET_FILE:aero_ext_gain>"
  AERO_EXT_BADABI_SO="$<TARGET_FILE:aero_ext_badabi>")
add_dependencies(native_extension aero_ext_gain aero_ext_badabi)
add_test(NAME native_extension COMMAND native_extension)
set_tests_properties(native_extension PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-6 DSL gate (008 §6): the ExprRule grammar evaluates + routes correctly, malformed exprs are
# rejected at configure (no crash), hot eval is 0-alloc (parse-once), and deploy accepts/rejects rules.
add_executable(expr_rule expr_rule.cpp)
target_link_libraries(expr_rule PRIVATE aero::runtime aero::nodes)
add_test(NAME expr_rule COMMAND expr_rule)
set_tests_properties(expr_rule PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-6 WASM-seam gate (008 §3, E1/E3/E4): the IWasmRuntime interface + capability/fuel types compile
# and NullWasmRuntime returns the documented gate error — the honest seam with no faked execution (R5).
add_executable(wasm_seam wasm_seam.cpp)
target_link_libraries(wasm_seam PRIVATE aero::ext)
add_test(NAME wasm_seam COMMAND wasm_seam)
set_tests_properties(wasm_seam PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- Phase-7: transport plugins (014) --------------------------------------------------------------
# Phase-7 LOAD-BEARING gate (014 §3 C1 / §5, X2): the FIFO/dedup resequencing shim. A ChaosChannel
# reorders + duplicates a seq-stamped stream; the CONTROL asserts the raw stream is NOT FIFO (shim is
# non-vacuous), then the Resequencer restores strict FIFO, exactly-once, 0 loss/dup/inversion/overflow.
add_executable(resequencer resequencer.cpp)
target_link_libraries(resequencer PRIVATE aero::transport)
add_test(NAME resequencer COMMAND resequencer)
set_tests_properties(resequencer PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-7 cross-node gate (014 §2 X1): two actors on distinct NodeIds exchange a message over Quark's
# REAL LoopbackTransport + DistributedRouter (fully-wired in-process path) — delivery + per-sender FIFO.
add_executable(cross_node_message cross_node_message.cpp)
target_link_libraries(cross_node_message PRIVATE quark::quark)
add_test(NAME cross_node_message COMMAND cross_node_message)
set_tests_properties(cross_node_message PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-7 gate (014 §5/§6 R5, §8): NullMqtt/NullGrpc return the documented offline gate error and the
# per-link TransportSelector picks the correct transport among available ones (relay when none shared).
add_executable(transport_gate transport_gate.cpp)
target_link_libraries(transport_gate PRIVATE aero::transport)
add_test(NAME transport_gate COMMAND transport_gate)
set_tests_properties(transport_gate PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- Phase-8: distribution & horizontal scale (010) ------------------------------------------------
# Phase-8 gate (010 §2): the device-affinity placement POLICY over Quark's Weighted-HRW — a device
# actor is placed on an ELIGIBLE node (affinity respected), a device needing a capability no node has is
# unplaceable as a clear value, and placements SPREAD across eligible nodes (not all on one).
add_executable(placement_affinity placement_affinity.cpp)
target_link_libraries(placement_affinity PRIVATE aero::cluster)
add_test(NAME placement_affinity COMMAND placement_affinity)
set_tests_properties(placement_affinity PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-8 LOAD-BEARING gate (010 §3): FENCED MIGRATION — the no-dual-drive guarantee. An actor migrates
# A→B; B recovers A's state + a FRESH Quark 012 fence (f2>f1), A's stale write with f1 is REJECTED by the
# store, B's write with f2 is accepted, and the driver is re-opened on re-activation. TSan-clean.
add_executable(fenced_migration fenced_migration.cpp)
target_link_libraries(fenced_migration PRIVATE aero::cluster aero::drivers)
add_test(NAME fenced_migration COMMAND fenced_migration)
set_tests_properties(fenced_migration PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-8 gate (010 §2.2): 90 device actors in 3 capability classes across 6 nodes — affinity respected
# per class, no eligible node starved, load shared within each eligible pair (not pinned to one node).
add_executable(cluster_place cluster_place.cpp)
target_link_libraries(cluster_place PRIVATE aero::cluster)
add_test(NAME cluster_place COMMAND cluster_place)
set_tests_properties(cluster_place PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- Phase-10: MES hook, Firmware OTA, protocol/node breadth (011/012/005) -------------------------
# Phase-10 LOAD-BEARING gate #1 (012 §3, M3): the transactional outbox. A flow stages a
# ProductionFinished report through the MesGateway actor; with the mock MES DOWN (503) the report is
# durably RETAINED in the outbox (proven to survive a simulated gateway restart — reload from the same
# store); bring the MES UP, drain, and assert it received the report EXACTLY once (a duplicate drain is
# deduped by the idempotency key). Ephemeral-port local httplib MES, bounded polling, clean shutdown.
add_executable(mes_outbox mes_outbox.cpp)
target_link_libraries(mes_outbox PRIVATE aero::mes aero::runtime)
add_test(NAME mes_outbox COMMAND mes_outbox)
set_tests_properties(mes_outbox PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-10 LOAD-BEARING gate #2 (011 §3/§4, O1/O2/O5): OTA. A good signed image → canary → COMMIT
# (device version advances); a forced health-check FAILURE → auto-ROLLBACK (device stays on the old
# version, rollback target recorded); an unsigned/tampered image is REJECTED before any transfer (O1).
# Fleet: a failing canary wave AUTO-PAUSES the rollout (O5) and later devices keep their firmware.
add_executable(ota_rollout ota_rollout.cpp)
target_link_libraries(ota_rollout PRIVATE aero::ota)
add_test(NAME ota_rollout COMMAND ota_rollout)
set_tests_properties(ota_rollout PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-10 breadth gate (005 §2): the new compute nodes each transform correctly and malformed input is
# handled without a crash (odd Modbus frame → Error; bad JSON → Error; CRC/mean/minmax/sum values).
add_executable(breadth_nodes breadth_nodes.cpp)
target_link_libraries(breadth_nodes PRIVATE aero::nodes aero::runtime)
add_test(NAME breadth_nodes COMMAND breadth_nodes)
set_tests_properties(breadth_nodes PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
