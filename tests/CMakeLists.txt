# AeroEdge tests. Deterministic, exit-code-gated (like Quark samples): 0 = pass.
add_executable(walking_skeleton integration/walking_skeleton.cpp)
target_link_libraries(walking_skeleton PRIVATE aero::core aero::nodes quark::quark)
add_test(NAME walking_skeleton COMMAND walking_skeleton)
set_tests_properties(walking_skeleton PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Invariant gate: the flow-execute path is 0-alloc on the steady path (I3). Not a benchmark.
# It replaces global operator new/delete to count allocations — which collides with clang's TSan C++
# runtime (libclang_rt.tsan_cxx also defines them → "multiple definition" link error). The gate is a
# deterministic single-thread allocation count with no concurrency to check, so it does not belong in
# the thread-sanitizer build; it still runs in the Release + ASan cells on both compilers.
if(NOT AEROEDGE_SANITIZER MATCHES "thread")
  add_executable(flow_zero_alloc core/flow_zero_alloc.cpp)
  target_link_libraries(flow_zero_alloc PRIVATE aero::core aero::nodes)
  add_test(NAME flow_zero_alloc COMMAND flow_zero_alloc)
  set_tests_properties(flow_zero_alloc PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
endif()

# Phase-2 ingestion gate: a driver streams N frames through the Quark 024 StreamChannel into an
# actor's flow — cross-thread SPSC — asserting FIFO, no loss, no dup, and payload integrity (006 §2).
add_executable(stream_ingest drivers/stream_ingest.cpp)
target_link_libraries(stream_ingest PRIVATE aero::core aero::nodes aero::drivers quark::quark)
add_test(NAME stream_ingest COMMAND stream_ingest)
set_tests_properties(stream_ingest PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-2 backpressure gate (the load-bearing one, 006 §3 "backpressure, not shedding"): a fast
# producer against a small ring + slow drain — 0 dropped frames, bounded occupancy, stalls observed.
add_executable(stream_backpressure drivers/stream_backpressure.cpp)
target_link_libraries(stream_backpressure PRIVATE aero::core aero::nodes aero::drivers quark::quark)
add_test(NAME stream_backpressure COMMAND stream_backpressure)
set_tests_properties(stream_backpressure PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-3 load-bearing gate (007 §2/§6/§7): durable actor state survives a simulated restart — a
# fresh actor recovers its snapshot from the SAME Store (InMemoryStore + crash-durable FileStore),
# while a stateful node's transient window rebuilds cold ("nodes compute, actors remember").
add_executable(persistence_recovery core/persistence_recovery.cpp)
target_link_libraries(persistence_recovery PRIVATE aero::core aero::nodes quark::quark)
add_test(NAME persistence_recovery COMMAND persistence_recovery)
set_tests_properties(persistence_recovery PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-3 stateful-node gate (007 §6): engine-free proof that transient node state is lost + rebuilt
# cold on restart while promoted (durable) actor state survives (S6).
add_executable(stateful_node core/stateful_node.cpp)
target_link_libraries(stateful_node PRIVATE aero::core aero::nodes)
add_test(NAME stateful_node COMMAND stateful_node)
set_tests_properties(stateful_node PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-4 schema gate (009 §2, 013 T3): parse a well-formed Application → assert fields; malformed /
# incomplete JSON → a clean error value (no throw), so the API can 4xx instead of aborting.
add_executable(schema_load schema/schema_load.cpp)
target_link_libraries(schema_load PRIVATE aero::schema)
add_test(NAME schema_load COMMAND schema_load)
set_tests_properties(schema_load PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-4 LOAD-BEARING gate (009): in-process, NO socket — deploy a declarative Application into a
# Runtime and assert the compiled flow runs every ingested frame (declarative deploy → running flow).
add_executable(runtime_deploy runtime/runtime_deploy.cpp)
target_link_libraries(runtime_deploy PRIVATE aero::runtime)
add_test(NAME runtime_deploy COMMAND runtime_deploy)
set_tests_properties(runtime_deploy PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# M9.2 LOAD-BEARING gate (006 §6.1, 018): proves Runtime::deploy() actually drives a PULL (poll()-only)
# driver end to end now, not just push — before this, ModbusTcpDriver/ModbusRtuDriver/OpcUaDriver were
# registered in the driver factory but could NOT be deployed via a real Application (the deploy path
# only ever spawned a run() producer thread, and all three have run() as a hard Unsupported). Deploys
# "aero.driver.modbus_tcp" against a hand-rolled fake Modbus-TCP server and asserts frames_processed
# climbs over real wall-clock time at the configured rate_hz, plus clean teardown (poller thread joins).
add_executable(runtime_poll_driver_wiring runtime/runtime_poll_driver_wiring.cpp)
target_link_libraries(runtime_poll_driver_wiring PRIVATE aero::runtime quark::quark Threads::Threads)
add_test(NAME runtime_poll_driver_wiring COMMAND runtime_poll_driver_wiring)
set_tests_properties(runtime_poll_driver_wiring PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-4 API gate (013 §5/§9): start the REST server on an ephemeral port, POST an Application + GET
# /status over a real socket, assert it is reported, then shut down cleanly. Wait-for-ready by polling.
add_executable(api_integration api/api_integration.cpp)
target_link_libraries(api_integration PRIVATE aero::api)
add_test(NAME api_integration COMMAND api_integration)
set_tests_properties(api_integration PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-5 validation gate (009 §3 P1, 004 §2.1): the Flow Compiler validates at deploy — invalid
# Applications (unknown type_id, empty flow, no source/output, bad config) are rejected as values
# (no crash, no half-deploy); a valid one is accepted.
add_executable(flow_validation core/flow_validation.cpp)
target_link_libraries(flow_validation PRIVATE aero::runtime)
add_test(NAME flow_validation COMMAND flow_validation)
set_tests_properties(flow_validation PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-5 LOAD-BEARING gate (009 §4 P2): hot-reload a live actor's flow while Commands flow — 0
# dropped, 0 duplicated (exactly-once), the pre-reload frames run the old flow and post-reload the
# new one, proven by an exact frame count + output-sum split. Mailbox-ordered swap on a Sequential
# actor (Quark 001/ADR-002 FIFO). Must be TSan-clean.
add_executable(hot_reload runtime/hot_reload.cpp)
target_link_libraries(hot_reload PRIVATE aero::runtime)
add_test(NAME hot_reload COMMAND hot_reload)
set_tests_properties(hot_reload PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-5 classification gate (009 §4 P3): a Live change (node config) hot-reloads; a BuildOnly change
# (actor key, persistence mode) is rejected with a clear error and never half-applied.
add_executable(reload_buildonly runtime/reload_buildonly.cpp)
target_link_libraries(reload_buildonly PRIVATE aero::runtime)
add_test(NAME reload_buildonly COMMAND reload_buildonly)
set_tests_properties(reload_buildonly PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-5 versioning gate (009 §6): reload A->B then rollback returns behavior to A.
add_executable(rollback runtime/rollback.cpp)
target_link_libraries(rollback PRIVATE aero::runtime)
add_test(NAME rollback COMMAND rollback)
set_tests_properties(rollback PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-5 API gate (009 §4/§6, 013 §5): PUT /apps/{name} (reload) + POST /apps/{name}/rollback over a
# real socket; a BuildOnly reload is a clean 400. Ephemeral port, health-poll, clean shutdown.
add_executable(api_reload api/api_reload.cpp)
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
add_executable(native_extension ext/native_extension.cpp)
target_link_libraries(native_extension PRIVATE aero::runtime aero::ext)
target_compile_definitions(native_extension PRIVATE
  AERO_EXT_GAIN_SO="$<TARGET_FILE:aero_ext_gain>"
  AERO_EXT_BADABI_SO="$<TARGET_FILE:aero_ext_badabi>")
add_dependencies(native_extension aero_ext_gain aero_ext_badabi)
add_test(NAME native_extension COMMAND native_extension)
set_tests_properties(native_extension PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-6 DSL gate (008 §6): the ExprRule grammar evaluates + routes correctly, malformed exprs are
# rejected at configure (no crash), hot eval is 0-alloc (parse-once), and deploy accepts/rejects rules.
# Overrides global operator new for the 0-alloc check, which collides with clang's TSan C++ runtime
# (see flow_zero_alloc above) — its logic is single-threaded, so skip it under -fsanitize=thread.
if(NOT AEROEDGE_SANITIZER MATCHES "thread")
  add_executable(expr_rule nodes/expr_rule.cpp)
  target_link_libraries(expr_rule PRIVATE aero::runtime aero::nodes)
  add_test(NAME expr_rule COMMAND expr_rule)
  set_tests_properties(expr_rule PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
endif()

# Phase-6 WASM-seam gate (008 §3, E1/E3/E4): the IWasmRuntime interface + capability/fuel types compile
# and NullWasmRuntime returns the documented gate error — the honest seam with no faked execution (R5).
add_executable(wasm_seam ext/wasm_seam.cpp)
target_link_libraries(wasm_seam PRIVATE aero::ext)
add_test(NAME wasm_seam COMMAND wasm_seam)
set_tests_properties(wasm_seam PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- Phase-7: transport plugins (014) --------------------------------------------------------------
# Phase-7 LOAD-BEARING gate (014 §3 C1 / §5, X2): the FIFO/dedup resequencing shim. A ChaosChannel
# reorders + duplicates a seq-stamped stream; the CONTROL asserts the raw stream is NOT FIFO (shim is
# non-vacuous), then the Resequencer restores strict FIFO, exactly-once, 0 loss/dup/inversion/overflow.
add_executable(resequencer transport/resequencer.cpp)
target_link_libraries(resequencer PRIVATE aero::transport)
add_test(NAME resequencer COMMAND resequencer)
set_tests_properties(resequencer PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-7 cross-node gate (014 §2 X1): two actors on distinct NodeIds exchange a message over Quark's
# REAL LoopbackTransport + DistributedRouter (fully-wired in-process path) — delivery + per-sender FIFO.
add_executable(cross_node_message transport/cross_node_message.cpp)
target_link_libraries(cross_node_message PRIVATE quark::quark)
add_test(NAME cross_node_message COMMAND cross_node_message)
set_tests_properties(cross_node_message PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-7 gate (014 §5/§6 R5, §8): NullMqtt/NullGrpc return the documented offline gate error and the
# per-link TransportSelector picks the correct transport among available ones (relay when none shared).
add_executable(transport_gate transport/transport_gate.cpp)
target_link_libraries(transport_gate PRIVATE aero::transport)
add_test(NAME transport_gate COMMAND transport_gate)
set_tests_properties(transport_gate PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-7 REAL-TCP gate (014 §4, X1): two nodes exchange genuine MessageFrames over an ACTUAL loopback
# TCP socket (POSIX, no external backend) — round-trip delivery both directions, header fidelity (C4),
# and per-(from → target) FIFO (C1) with no resequencer (TCP is the order-preserving substrate). The
# frame codec (frame_codec.hpp) is exercised end-to-end here. Concurrent accept/reader threads → TSan gate.
add_executable(tcp_transport transport/tcp_transport.cpp)
target_link_libraries(tcp_transport PRIVATE aero::transport)
add_test(NAME tcp_transport COMMAND tcp_transport)
set_tests_properties(tcp_transport PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# 017 Phase-1 gate: NativeBroker (AeroEdge's own embedded MQTT server) over real loopback sockets — a
# hand-rolled test client (built on the same shared mqtt_codec.hpp) proves CONNECT/SUBSCRIBE/PUBLISH/
# PUBACK, wildcard topic routing, retained-message replay, and the on_publish() ingestion seam. No
# external MQTT client/backend needed (single-node broker, unlike the REAL-MQTT gate below).
add_executable(native_broker broker/native_broker.cpp)
target_link_libraries(native_broker PRIVATE aero::broker)
add_test(NAME native_broker COMMAND native_broker)
set_tests_properties(native_broker PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# 017 §6 (revised scope) gate: bridge.hpp — bridging (MqttBridgeSink, HttpWebhookBridgeSink) + the
# BrokerRuleEngine reusing aero-nodes' ExprRuleNode (008 §6) as the content gate over on_publish().
# MqttBridgeSink is proven against a real NativeBroker; HttpWebhookBridgeSink against a local httplib
# server (same pattern as tests/mes/mes_outbox.cpp / tests/api/api_integration.cpp).
add_executable(bridge broker/bridge.cpp)
target_link_libraries(bridge PRIVATE aero::broker aero::nodes aero-thirdparty-httplib aero-thirdparty-json)
add_test(NAME bridge COMMAND bridge)
set_tests_properties(bridge PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# M8 gate (017 §6 Status): RabbitMqBridgeSink (broker/rabbitmq_bridge_sink.hpp) over rabbitmq-c — a
# fourth IBridgeSink alongside MqttBridgeSink/HttpWebhookBridgeSink. rabbitmq-c is client-only (ships no
# test/mock broker), so this test hand-rolls a minimal fake AMQP 0-9-1 server (same quark::pal::net +
# aero::pal::poll primitives every other socket test in this tree uses, mirroring
# tests/drivers/modbus_tcp_driver.cpp's FakeModbusServer shape) to drive a REAL RabbitMqBridgeSink through
# a full connect/login/channel-open/publish/close round trip, a refused-port connect() failure, and a
# mid-session connection kill proving the NEXT publish() fails fast, never hangs. Not gated on
# AERO_ENABLE_RABBITMQ (mirrors opcua_driver's own unconditional build below — consistent with this
# tree's existing precedent for an optional vendored dependency's test target): with the option OFF,
# RabbitMqBridgeSink's stub (rabbitmq_bridge_sink.hpp's #else branch) still compiles and links fine since
# this test only calls the IBridgeSink surface, never rabbitmq-c's own headers directly — it would just
# fail its assertions at runtime (stub connect()/publish() always return false), which the default-ON
# option means nobody normally hits.
add_executable(rabbitmq_bridge_sink broker/rabbitmq_bridge_sink.cpp)
target_link_libraries(rabbitmq_bridge_sink PRIVATE aero::broker)
add_test(NAME rabbitmq_bridge_sink COMMAND rabbitmq_bridge_sink)
set_tests_properties(rabbitmq_bridge_sink PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# 017 M6 gate: broker_cluster.hpp — cross-node PUBLISH broadcast over REAL loopback TCP. Two
# NativeBroker+BrokerCluster pairs (aero::transport::TcpTransport under a Quark DistributedRouter,
# StaticBrokerMembership) prove the cross-node hop (a node-2 PUBLISH reaches a node-1 subscriber) and
# loop prevention (node 2's own subscriber sees its local PUBLISH exactly once, never a relayed-back
# duplicate). Same aero::broker library as native_broker/bridge above — no new link target needed.
add_executable(broker_cluster broker/broker_cluster.cpp)
target_link_libraries(broker_cluster PRIVATE aero::broker)
add_test(NAME broker_cluster COMMAND broker_cluster)
set_tests_properties(broker_cluster PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# M5 TLS+ACL milestone gate: acl.hpp — the broker-local per-topic Authorizer seam (Allow/Deny-all
# triviality, TopicAclAuthorizer's Closed/Open default posture, exact + wildcard principal/topic
# matching, first-match-wins ordering, Publish/Subscribe action independence). Pure logic, no sockets.
add_executable(acl broker/acl.cpp)
target_link_libraries(acl PRIVATE aero::broker)
add_test(NAME acl COMMAND acl)
set_tests_properties(acl PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# 017 M7 gate: MQTT 5 support in NativeBroker — protocol negotiation, CONNECT/Will/PUBLISH/SUBSCRIBE
# Properties parsing (mqtt_codec.hpp's read_varint/read_properties), and v5 CONNACK/SUBACK reason codes,
# over real loopback sockets (mirrors native_broker.cpp's TestClient pattern) plus standalone unit
# coverage of the Properties codec itself.
add_executable(mqtt5 broker/mqtt5.cpp)
target_link_libraries(mqtt5 PRIVATE aero::broker)
add_test(NAME mqtt5 COMMAND mqtt5)
set_tests_properties(mqtt5 PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# uv drives the Python-hosted backends (amqtt broker / grpcio server) the REAL MQTT & gRPC gates need.
# Absent uv, those tests SKIP (exit 0 with a SKIP line) rather than fail — the backend is external infra.
find_program(AERO_UV_BIN uv)
if(NOT AERO_UV_BIN)
  set(AERO_UV_BIN "uv")  # fall back to PATH lookup at runtime; the test skips cleanly if it is missing
  message(STATUS "uv not found at configure time; MQTT/gRPC transport tests will skip if it is unavailable")
endif()

# Phase-7 REAL-MQTT gate (014 §5, X2/X3): two nodes exchange genuine MessageFrames through a REAL amqtt
# broker (spawned via uv) using the hand-written MqttClientTransport — CONNECT/SUBSCRIBE/PUBLISH-QoS1/
# PUBACK, topic-per-node, and the C1 resequencing shim passing a real stream exactly-once + FIFO. Spawns
# a broker subprocess + reader threads → also a TSan gate. Longer timeout (may fetch amqtt on first run).
add_executable(mqtt_transport transport/mqtt_transport.cpp)
target_include_directories(mqtt_transport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(mqtt_transport PRIVATE aero::transport)
target_compile_definitions(mqtt_transport PRIVATE
  AERO_UV_BIN="${AERO_UV_BIN}"
  AERO_MQTT_BROKER_PY="${CMAKE_CURRENT_SOURCE_DIR}/py/mqtt_broker.py")
add_test(NAME mqtt_transport COMMAND mqtt_transport)
set_tests_properties(mqtt_transport PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL" TIMEOUT 120)

# Phase-7 REAL-gRPC gate (014 §6, X2): the hand-written GrpcClientTransport (its OWN HTTP/2 + gRPC framing,
# no gRPC library linked) exchanges genuine MessageFrames with a REAL grpcio bidi server (spawned via uv)
# over an actual HTTP/2 stream — the server decodes each frame with AeroEdge's codec + echoes swapped,
# proving a full round-trip with single-stream FIFO (no resequencer). Reader thread → TSan gate. Longer
# timeout (may fetch grpcio on first run).
add_executable(grpc_transport transport/grpc_transport.cpp)
target_include_directories(grpc_transport PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(grpc_transport PRIVATE aero::transport)
target_compile_definitions(grpc_transport PRIVATE
  AERO_UV_BIN="${AERO_UV_BIN}"
  AERO_GRPC_SERVER_PY="${CMAKE_CURRENT_SOURCE_DIR}/py/grpc_echo_server.py")
add_test(NAME grpc_transport COMMAND grpc_transport)
set_tests_properties(grpc_transport PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL" TIMEOUT 120)

# ---- Phase-8: distribution & horizontal scale (010) ------------------------------------------------
# Phase-8 gate (010 §2): the device-affinity placement POLICY over Quark's Weighted-HRW — a device
# actor is placed on an ELIGIBLE node (affinity respected), a device needing a capability no node has is
# unplaceable as a clear value, and placements SPREAD across eligible nodes (not all on one).
add_executable(placement_affinity cluster/placement_affinity.cpp)
target_link_libraries(placement_affinity PRIVATE aero::cluster)
add_test(NAME placement_affinity COMMAND placement_affinity)
set_tests_properties(placement_affinity PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-8 LOAD-BEARING gate (010 §3): FENCED MIGRATION — the no-dual-drive guarantee. An actor migrates
# A→B; B recovers A's state + a FRESH Quark 012 fence (f2>f1), A's stale write with f1 is REJECTED by the
# store, B's write with f2 is accepted, and the driver is re-opened on re-activation. TSan-clean.
add_executable(fenced_migration cluster/fenced_migration.cpp)
target_link_libraries(fenced_migration PRIVATE aero::cluster aero::drivers)
add_test(NAME fenced_migration COMMAND fenced_migration)
set_tests_properties(fenced_migration PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-8 gate (010 §2.2): 90 device actors in 3 capability classes across 6 nodes — affinity respected
# per class, no eligible node starved, load shared within each eligible pair (not pinned to one node).
add_executable(cluster_place cluster/cluster_place.cpp)
target_link_libraries(cluster_place PRIVATE aero::cluster)
add_test(NAME cluster_place COMMAND cluster_place)
set_tests_properties(cluster_place PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- Phase-10: MES hook, Firmware OTA, protocol/node breadth (011/012/005) -------------------------
# Phase-10 LOAD-BEARING gate #1 (012 §3, M3): the transactional outbox. A flow stages a
# ProductionFinished report through the MesGateway actor; with the mock MES DOWN (503) the report is
# durably RETAINED in the outbox (proven to survive a simulated gateway restart — reload from the same
# store); bring the MES UP, drain, and assert it received the report EXACTLY once (a duplicate drain is
# deduped by the idempotency key). Ephemeral-port local httplib MES, bounded polling, clean shutdown.
add_executable(mes_outbox mes/mes_outbox.cpp)
target_link_libraries(mes_outbox PRIVATE aero::mes aero::runtime)
add_test(NAME mes_outbox COMMAND mes_outbox)
set_tests_properties(mes_outbox PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-11.1 gate (016 §2.3): the MES gateway wired into Runtime + reachable over the real REST API —
# GET /mes/outbox is {"configured": false} until configure_mes(), then reflects a staged/retained
# report (MES down) and a drained/delivered one (MES up, via POST /mes/outbox/drain).
add_executable(runtime_mes_wiring runtime/runtime_mes_wiring.cpp)
target_link_libraries(runtime_mes_wiring PRIVATE aero::api)
add_test(NAME runtime_mes_wiring COMMAND runtime_mes_wiring)
set_tests_properties(runtime_mes_wiring PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-10 LOAD-BEARING gate #2 (011 §3/§4, O1/O2/O5): OTA. A good signed image → canary → COMMIT
# (device version advances); a forced health-check FAILURE → auto-ROLLBACK (device stays on the old
# version, rollback target recorded); an unsigned/tampered image is REJECTED before any transfer (O1).
# Fleet: a failing canary wave AUTO-PAUSES the rollout (O5) and later devices keep their firmware.
add_executable(ota_rollout ota/ota_rollout.cpp)
target_link_libraries(ota_rollout PRIVATE aero::ota)
add_test(NAME ota_rollout COMMAND ota_rollout)
set_tests_properties(ota_rollout PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-11.2 gate (016 §2.1/§2.2): Cluster+Fleet(OTA) wired into Runtime + reachable over the real REST
# API — GET /fleet and GET /ota/rollouts are {"configured": false} until configure_fleet(), then
# reflect real placement (010) and a real wave-by-wave rollout (011) against the registered devices.
add_executable(runtime_fleet_wiring runtime/runtime_fleet_wiring.cpp)
target_link_libraries(runtime_fleet_wiring PRIVATE aero::api)
add_test(NAME runtime_fleet_wiring COMMAND runtime_fleet_wiring)
set_tests_properties(runtime_fleet_wiring PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Phase-10 breadth gate (005 §2): the new compute nodes each transform correctly and malformed input is
# handled without a crash (odd Modbus frame → Error; bad JSON → Error; CRC/mean/minmax/sum values).
add_executable(breadth_nodes nodes/breadth_nodes.cpp)
target_link_libraries(breadth_nodes PRIVATE aero::nodes aero::runtime)
add_test(NAME breadth_nodes COMMAND breadth_nodes)
set_tests_properties(breadth_nodes PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- M5: TLS+ACL milestone — non-blocking TLS byte-channel PAL (include/aero/pal/tls.hpp) -----------
# `TlsServerContext`/`TlsSession` proven over a REAL loopback TCP socket against mbedTLS's own CLIENT
# APIs (tls.hpp itself is server-only — AeroEdge's broker is always the TLS server): a genuine non-
# blocking handshake completes and data round-trips through recv_some/send_some (server-auth-only TLS),
# mTLS accepts a client cert signed by the configured ca_file, and mTLS REJECTS a connection with no
# client cert when one is required. Also smoke-checks io_channel.hpp's PlainChannel/TlsChannel surface.
# Uses the pre-generated, checked-in TEST-ONLY certs under pal/certs/ (never used in production).
add_executable(tls_channel pal/tls_channel.cpp)
target_link_libraries(tls_channel PRIVATE aero::sdk Threads::Threads aero-thirdparty-tls)
target_compile_definitions(tls_channel PRIVATE
  AERO_TLS_TEST_CERTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pal/certs")
add_test(NAME tls_channel COMMAND tls_channel)
set_tests_properties(tls_channel PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# M5 TLS+ACL milestone INTEGRATION gate: NativeBroker itself wired to TLS (aero/pal/tls.hpp) + the ACL
# seam (aero/broker/acl.hpp) — as opposed to tls_channel (TLS PAL in isolation) and acl (ACL policy logic
# in isolation), this proves the WIRING: a real NativeBroker with cfg.tls/cfg.authenticate/cfg.authorizer
# set, driven by real MQTT clients over real sockets (one TLS-capable, hand-rolled over mbedTLS's
# client-role APIs, mirroring tls_channel.cpp's own TestTlsClient; one plaintext, mirroring
# native_broker.cpp's TestClient). Reuses tests/pal/certs/ directly (same AERO_TLS_TEST_CERTS_DIR
# compile definition as tls_channel, rather than duplicating cert files) and links aero-thirdparty-tls
# for the same reason tls_channel does (real mbedTLS client-role calls in the test's own TLS client code).
add_executable(native_broker_security broker/native_broker_security.cpp)
target_link_libraries(native_broker_security PRIVATE aero::broker aero-thirdparty-tls)
target_compile_definitions(native_broker_security PRIVATE
  AERO_TLS_TEST_CERTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/pal/certs")
add_test(NAME native_broker_security COMMAND native_broker_security)
set_tests_properties(native_broker_security PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- M9a: multi-protocol southbound (018) ------------------------------------------------------------
# ModbusTcpDriver (drivers/modbus_tcp_driver.hpp): a real Modbus-TCP PULL driver (FC03 Read Holding
# Registers) over aero::pal::dial_tcp (Part 2) + a bounded-backoff reconnect (006 §8, first driver in
# this tree to actually implement it). A hand-rolled fake Modbus-TCP server (same quark::pal::net +
# aero::pal::poll primitives every other socket test in this tree uses) proves: (1) happy path — poll()
# + ModbusDecodeNode (nodes/compute_nodes.hpp, reused not reimplemented) decode the canned register
# values correctly; (2) an oversized register_count is rejected at open(), never silently truncated
# (Part 1's Frame::payload_len/kMaxFramePayload budget); (3) connection loss mid-session is recovered by
# a later poll() reconnecting, never wedging or crashing.
add_executable(modbus_tcp_driver drivers/modbus_tcp_driver.cpp)
target_link_libraries(modbus_tcp_driver PRIVATE aero::drivers aero::nodes aero::sdk quark::quark Threads::Threads)
add_test(NAME modbus_tcp_driver COMMAND modbus_tcp_driver)
set_tests_properties(modbus_tcp_driver PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- M9b: OPC-UA southbound (018) ---------------------------------------------------------------------
# OpcUaDriver (drivers/opcua_driver.hpp): a real OPC-UA client PULL driver over open62541 (v1 scope: no
# security, no Subscriptions, poll-configured-NodeIds only). A REAL open62541 UA_Server, hosted
# in-process on a background thread (open62541's own standard test idiom — no external test infra
# needed), proves: (1) happy path — poll() + JsonParseNode (nodes/compute_nodes.hpp, reused not
# reimplemented — this driver serializes flat JSON precisely so JsonParseNode needs zero changes) decode
# the canned variable-node values correctly; (2) a node_ids list too large for the 128-byte payload
# budget is rejected at open(), never a silently truncated/corrupted frame; (3) connection loss
# mid-session (the fake server stopped entirely) is recovered by a later poll() reconnecting once the
# server comes back, never wedging or crashing. Links aero-thirdparty-opcua directly (not just
# transitively via aero::drivers) because the test's OWN fake server code calls real UA_Server APIs.
add_executable(opcua_driver drivers/opcua_driver.cpp)
target_link_libraries(opcua_driver PRIVATE aero::drivers aero::nodes aero::sdk aero-thirdparty-opcua
  quark::quark Threads::Threads)
add_test(NAME opcua_driver COMMAND opcua_driver)
set_tests_properties(opcua_driver PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- M9.3: OPC-UA Subscriptions/MonitoredItems (018 §8) ------------------------------------------------
# OpcUaSubscriptionDriver (drivers/opcua_subscription_driver.hpp): the PUSH counterpart to OpcUaDriver, a
# real open62541 UA_Server (own fixed port 48712, distinct from opcua_driver's 48711) with a background
# "updater" thread bumping a Counter variable proves: (1) run() connects, subscribes, and delivers
# multiple DISTINCT DataChange notifications, decoded correctly by the same JsonParseNode poll mode uses;
# (2) a malformed NodeId is rejected at open() with no network I/O (this driver never dials in open());
# (3) connection loss mid-subscription is survived and reconnected (bounded backoff, 006 §8).
add_executable(opcua_subscription_driver drivers/opcua_subscription_driver.cpp)
target_link_libraries(opcua_subscription_driver PRIVATE aero::drivers aero::nodes aero::sdk
  aero-thirdparty-opcua quark::quark Threads::Threads)
add_test(NAME opcua_subscription_driver COMMAND opcua_subscription_driver)
set_tests_properties(opcua_subscription_driver PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- M9.4: OPC-UA security policies (018 §8, the last v1 backlog item) --------------------------------
# Sign/SignAndEncrypt over a client certificate + cert-based client auth (opcua_security.hpp), proven
# against a REAL open62541 UA_Server configured via UA_ServerConfig_setDefaultWithSecurityPolicies (own
# fixed port 48715, distinct from every other OPC-UA test's port). Uses pre-generated, checked-in
# TEST-ONLY self-signed certs under drivers/certs/ (never used in production) — mirrors tls_channel/
# native_broker_security's own AERO_TLS_TEST_CERTS_DIR convention.
add_executable(opcua_driver_security drivers/opcua_driver_security.cpp)
target_link_libraries(opcua_driver_security PRIVATE aero::drivers aero::nodes aero::sdk aero-thirdparty-opcua
  aero-thirdparty-tls quark::quark Threads::Threads)
target_compile_definitions(opcua_driver_security PRIVATE
  AERO_OPCUA_TEST_CERTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/drivers/certs")
add_test(NAME opcua_driver_security COMMAND opcua_driver_security)
set_tests_properties(opcua_driver_security PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# ---- M9.1 PR H: Modbus RTU / serial southbound (018 §8) -----------------------------------------------
# ModbusRtuDriver (drivers/modbus_rtu_driver.hpp): the serial counterpart to ModbusTcpDriver, over a new
# aero::pal::serial PAL (this tree's first serial I/O of any kind). No portable hardware-free way to fake
# a COM port exists (unlike a TCP loopback server), so the driver talks to an ISerialTransport seam
# instead of the PAL directly; this test injects an in-memory fake transport that echoes canned RTU
# frames (built independently in the test file, mirroring FakeModbusServer's own arm's-length approach)
# to prove: (1) happy path read (FC03) decodes correctly and the driver's own request bytes/CRC are
# correct; (2) a write (FC06) round-trip; (3) a Modbus exception response is a clean Error with the
# transport left open; (4) a CRC-corrupted response is a clean Error with the transport closed
# (reconnect-eligible); (5) modbus_crc16 itself matches the Modbus spec's own worked example.
add_executable(modbus_rtu_driver drivers/modbus_rtu_driver.cpp)
target_link_libraries(modbus_rtu_driver PRIVATE aero::drivers aero::nodes aero::sdk quark::quark Threads::Threads)
add_test(NAME modbus_rtu_driver COMMAND modbus_rtu_driver)
set_tests_properties(modbus_rtu_driver PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
