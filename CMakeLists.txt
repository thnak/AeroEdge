cmake_minimum_required(VERSION 3.24)
project(AeroEdge LANGUAGES CXX VERSION 0.1.0
        DESCRIPTION "AeroEdge — industrial edge platform on the QuarkCpp actor engine")

# ---- Standard --------------------------------------------------------------
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---- Optional sanitizer (CI matrix: address,undefined | thread) ------------
set(AEROEDGE_SANITIZER "" CACHE STRING "Sanitizer passed to -fsanitize (e.g. address,undefined | thread)")
if(AEROEDGE_SANITIZER)
  add_compile_options(-fsanitize=${AEROEDGE_SANITIZER} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${AEROEDGE_SANITIZER})
endif()

# ---- Threads (httplib + Quark worker lanes need pthreads) ------------------
find_package(Threads REQUIRED)

# ---- Pair the C compiler with CMAKE_CXX_COMPILER, if not already set -------
# AeroEdge itself is pure C++ (LANGUAGES CXX above) and never triggered CMake's C-language detection
# before mbedTLS (below): mbedTLS's own root CMakeLists.txt calls `project(MbedTLS C)`, so the FIRST
# time C is ever probed in this build is inside that FetchContent_MakeAvailable call, lazily. Left to
# its own devices, CMake probes whatever `cc` resolves to on PATH — on a system where that's GCC while
# CMAKE_CXX_COMPILER is Clang (e.g. this project's CI matrix, which builds several legs with clang++-N
# and passes Clang-only flags like `--gcc-install-dir=...` through CMAKE_CXX_FLAGS/CMAKE_EXE_LINKER_FLAGS
# — the LATTER is language-agnostic and so also reaches the C-compiler-ABI probe), the probe can fail
# outright (GCC's `cc` doesn't understand a Clang driver flag) or silently mix toolchains. Deriving a
# same-family, same-version C compiler from CMAKE_CXX_COMPILER before mbedTLS ever asks for one avoids
# both — this only fires when the caller didn't already pin CMAKE_C_COMPILER explicitly.
if(NOT CMAKE_C_COMPILER)
  get_filename_component(_aero_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  get_filename_component(_aero_cxx_name "${CMAKE_CXX_COMPILER}" NAME)
  set(_aero_c_name "")
  if(_aero_cxx_name MATCHES "^clang\\+\\+(.*)$")
    set(_aero_c_name "clang${CMAKE_MATCH_1}")
  elseif(_aero_cxx_name MATCHES "^g\\+\\+(.*)$")
    set(_aero_c_name "gcc${CMAKE_MATCH_1}")
  endif()
  if(_aero_c_name)
    if(_aero_cxx_dir)
      set(CMAKE_C_COMPILER "${_aero_cxx_dir}/${_aero_c_name}" CACHE STRING
          "C compiler, paired with CMAKE_CXX_COMPILER" FORCE)
    else()
      set(CMAKE_C_COMPILER "${_aero_c_name}" CACHE STRING
          "C compiler, paired with CMAKE_CXX_COMPILER" FORCE)
    endif()
  endif()
endif()

# ---- Pin CMAKE_AR/CMAKE_RANLIB to plain binutils -------------------------
# M9b (open62541 vendoring) surfaced a gap this same C-compiler-pairing fix (above) didn't cover: CMake's
# Clang toolchain detection (CMakeFindBinUtils) prefers a co-located `llvm-ar`/`llvm-ranlib` over plain
# `ar`/`ranlib` WHEN ONE IS FOUND next to the compiler — which is environment-dependent (present on some
# clang installs, e.g. a from-source apt.llvm.org install, absent on others). A build where AeroEdge's own
# archives and mbedTLS's get built with one `ar` while a FetchContent'd subproject's own internal probing
# resolves a DIFFERENT one (or vice versa) risks producing archives one linker invocation can't read
# ("file format not recognized") — the same class of cross-toolchain mismatch the C-compiler pairing above
# exists to prevent, just for the archiver instead of the compiler.
#
# UNCONDITIONAL, unlike the C-compiler pairing above: `project(AeroEdge LANGUAGES CXX ...)` on line 2 —
# the very first line of this file — already runs CMake's full CXX toolchain detection, which resolves
# and CACHEs CMAKE_AR/CMAKE_RANLIB as a side effect (CMakeFindBinUtils, invoked from
# CMakeDetermineCXXCompiler). By the time ANY later line in this file runs, CMAKE_AR is therefore ALREADY
# non-empty — an `if(NOT CMAKE_AR)` guard here is always false and silently never fires (this was the
# first, broken version of this fix: it built and tested clean locally because the local repro's `ar`
# happened to already resolve to plain binutils, then still failed in CI as "file format not recognized",
# exactly the failure this block exists to prevent — CI's clang-20 install DOES have a co-located
# `llvm-ar`/`llvm-ranlib` CMake's auto-detection picked instead). Force it every configure instead.
find_program(_aero_ar_path NAMES ar)
if(_aero_ar_path)
  set(CMAKE_AR "${_aero_ar_path}" CACHE FILEPATH "Archiver, pinned to plain binutils ar" FORCE)
endif()
find_program(_aero_ranlib_path NAMES ranlib)
if(_aero_ranlib_path)
  set(CMAKE_RANLIB "${_aero_ranlib_path}" CACHE FILEPATH "Archive indexer, pinned to plain binutils ranlib" FORCE)
endif()

# ---- TLS (mbedTLS, vendored via CMake FetchContent) -------------------------
# M5 (TLS+ACL milestone): the native MQTT broker's TLS byte-channel PAL (include/aero/pal/tls.hpp) needs
# a TLS library. mbedTLS is small, portable C designed for embedded/edge targets (unlike OpenSSL's much
# heavier footprint) — a better fit for this project than the alternatives. Fetched at configure time
# (not vendored as source under third_party/, unlike header-only httplib/nlohmann-json) because mbedTLS
# is a real C library that needs compiling, not just #include'ing.
option(AERO_ENABLE_TLS "Build with TLS support (mbedTLS, fetched via CMake FetchContent)" ON)
if(AERO_ENABLE_TLS)
  include(FetchContent)
  FetchContent_Declare(
    mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG        v3.6.7  # latest v3.6.x LTS tag at the time this was pinned (checked empirically)
    # See cmake/patch_mbedtls.cmake's banner: two independent fixes — (1) neutralizes a real mbedTLS
    # CMake false-positive (CMAKE_C_SIMULATE_ID=MSVC for Clang's GNU-style driver targeting the MSVC
    # ABI, this project's Windows toolchain) that otherwise appends cl.exe-only `/W3 /utf-8` flags
    # clang++.exe rejects; (2) disables MBEDTLS_SELF_TEST, whose unused self-test instrumentation
    # includes an unsynchronized global counter that TSan correctly flags as a real data race under
    # concurrent TLS handshakes.
    PATCH_COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/patch_mbedtls.cmake"
  )
  # Keep mbedTLS's own build lean/quiet, mirroring the QUARK_BUILD_TESTS/BENCH/SAMPLES OFF pattern right
  # below (add_subdirectory(QuarkCpp)) — force the cache vars mbedTLS's CMakeLists reads BEFORE
  # FetchContent_MakeAvailable pulls it in and configures it:
  #   ENABLE_TESTING / ENABLE_PROGRAMS OFF  — we only want the library, not mbedTLS's own test suite or
  #     example programs (ssl_client2, ssl_server2, ...), which would otherwise get added as build targets.
  #   MBEDTLS_FATAL_WARNINGS OFF — mbedTLS builds -Werror by default, tuned for ITS OWN CI compiler
  #     versions; on a compiler/flag combination it didn't anticipate that can fail the build over a
  #     warning that isn't actually AeroEdge's problem to fix in vendored code.
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(mbedtls)
endif()

# aero-thirdparty-tls — INTERFACE seam over mbedTLS (mirrors aero-thirdparty-httplib's shape below). With
# AERO_ENABLE_TLS on, links the real mbedTLS static targets — confirmed empirically against v3.6.7's
# library/CMakeLists.txt: it names them exactly `mbedtls`/`mbedx509`/`mbedcrypto` (also aliased as
# `MbedTLS::<name>`, but the bare names are what add_subdirectory/FetchContent actually registers) — and
# defines AERO_TLS_ENABLED=1 so include/aero/pal/tls.hpp can compile its real mbedTLS-backed
# implementation instead of the "not compiled in" stub. With AERO_ENABLE_TLS off this stays a genuine
# no-op INTERFACE target: no fetch attempted, no network access needed, nothing to link.
add_library(aero-thirdparty-tls INTERFACE)
if(AERO_ENABLE_TLS)
  target_link_libraries(aero-thirdparty-tls INTERFACE mbedtls mbedx509 mbedcrypto)
  target_compile_definitions(aero-thirdparty-tls INTERFACE AERO_TLS_ENABLED=1)
endif()

# ---- OPC-UA (open62541, vendored via CMake FetchContent) -------------------
# M9b (018 §Multi-protocol southbound): OpcUaDriver (aero/drivers/opcua_driver.hpp) is a pull-style
# IDriver over open62541 (github.com/open62541/open62541), the standard open-source C99 OPC-UA SDK
# (Mozilla Public License 2.0). v1 scope is a pure client-poll: connect to one known endpoint, read a
# configured set of NodeIds — NO security policies (Sign/SignAndEncrypt is filed as backlog, not an
# oversight), no Subscriptions/MonitoredItems, no browsing/method calls. Fetched (not vendored under
# third_party/) for the same reason as mbedTLS above: a real C library that needs compiling, not just
# #include'ing.
#
# NOTE: open62541 needs Python 3 at ITS OWN build time (its NodeSet/type code-generation step, see its
# `find_package(Python3 REQUIRED)`) — the one genuinely new implicit toolchain requirement this adds to
# the build; nothing else in this repo needs Python. A machine without a real Python 3 on PATH fails to
# CONFIGURE this dependency (not a link/compile error) — a human needs to install one.
option(AERO_ENABLE_OPCUA "Build with OPC-UA client support (open62541, fetched via CMake FetchContent)" ON)
if(AERO_ENABLE_OPCUA)
  include(FetchContent)
  FetchContent_Declare(
    open62541
    GIT_REPOSITORY https://github.com/open62541/open62541.git
    GIT_TAG        v1.4.11  # a real tagged v1.4.x LTS release — confirmed present via `git ls-remote
                             # --tags` against the upstream repo at the time this was pinned
  )
  # Keep open62541's own build lean/quiet, mirroring the mbedTLS ENABLE_TESTING/QUARK_BUILD_TESTS
  # pattern above — force the cache vars ITS CMakeLists.txt reads BEFORE FetchContent_MakeAvailable:
  #   UA_ENABLE_ENCRYPTION "OFF" — a STRING cache var upstream (not BOOL: "OFF" or a plugin name like
  #     "MBEDTLS"/"OPENSSL"), already OFF by default but forced explicitly here to document the v1 scope
  #     decision: NO security policies, no TLS/crypto coupling for this dependency.
  #   UA_BUILD_EXAMPLES / UA_BUILD_TOOLS / UA_BUILD_UNIT_TESTS OFF — only the client library, never
  #     open62541's own example servers/clients, shell tools, or unit test suite as build targets (NOTE:
  #     the real upstream option is UA_BUILD_UNIT_TESTS, not "UA_BUILD_TESTING" — verified against
  #     open62541's own root CMakeLists.txt).
  #   UA_ENABLE_DISCOVERY / UA_ENABLE_PUBSUB OFF — this driver dials one known endpoint directly (no LDS
  #     discovery) and never touches PubSub; both default ON upstream and add real build weight for a
  #     pure client-poll use case. UA_ENABLE_SUBSCRIPTIONS is left at its upstream default (ON): not
  #     needed for v1 client-poll usage, but nothing here requires forcing it off either.
  #   UA_ENABLE_PUBSUB_INFORMATIONMODEL OFF — forced alongside UA_ENABLE_PUBSUB: open62541's own
  #     CMakeLists.txt hard-errors ("PubSub information model representation cannot be used with disabled
  #     PubSub function") if this stays at ITS default (ON) while UA_ENABLE_PUBSUB is off — discovered
  #     empirically configuring this exact build.
  #   UA_ENABLE_DEBUG_SANITIZER OFF — defaults ON upstream, and when CMAKE_BUILD_TYPE is Debug (true for
  #     every CI sanitizer leg, address/undefined AND thread) open62541's own CMakeLists.txt unconditionally
  #     appends "-fsanitize=address -fsanitize=leak -fsanitize=undefined" to CMAKE_C_FLAGS/CMAKE_CXX_FLAGS
  #     for ITS OWN sources, regardless of AEROEDGE_SANITIZER above — on the thread (TSan) leg this collides
  #     head-on with this project's own -fsanitize=thread ("invalid argument '-fsanitize=address' not
  #     allowed with '-fsanitize=thread'", a hard compile error, not a warning). AeroEdge's own top-level
  #     AEROEDGE_SANITIZER option is this project's single source of truth for which sanitizer a CI leg
  #     builds under; a vendored dependency silently opting itself into a DIFFERENT one is never correct
  #     here, so this is forced off unconditionally, not just for the thread leg.
  set(UA_ENABLE_ENCRYPTION "OFF" CACHE STRING "" FORCE)
  set(UA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(UA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(UA_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(UA_ENABLE_DISCOVERY OFF CACHE BOOL "" FORCE)
  set(UA_ENABLE_PUBSUB OFF CACHE BOOL "" FORCE)
  set(UA_ENABLE_PUBSUB_INFORMATIONMODEL OFF CACHE BOOL "" FORCE)
  set(UA_ENABLE_DEBUG_SANITIZER OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(open62541)
endif()

# aero-thirdparty-opcua — INTERFACE seam over open62541 (mirrors aero-thirdparty-tls's shape above). With
# AERO_ENABLE_OPCUA on, links the real target open62541's own CMakeLists.txt registers — confirmed
# empirically against v1.4.11's src/CMakeLists.txt: `add_library(open62541 ...)` plus `add_library(
# open62541::open62541 ALIAS open62541)` — and defines AERO_OPCUA_ENABLED=1 so
# include/aero/drivers/opcua_driver.hpp can compile its real open62541-backed implementation instead of
# the "not compiled in" stub. With AERO_ENABLE_OPCUA off this stays a genuine no-op INTERFACE target: no
# fetch attempted, no network access needed, nothing to link.
add_library(aero-thirdparty-opcua INTERFACE)
if(AERO_ENABLE_OPCUA)
  target_link_libraries(aero-thirdparty-opcua INTERFACE open62541::open62541)
  target_compile_definitions(aero-thirdparty-opcua INTERFACE AERO_OPCUA_ENABLED=1)
  if(WIN32)
    # open62541's Win32 networking layer pulls in <windows.h> transitively; without NOMINMAX its global
    # min/max macros collide with std::min/std::max used elsewhere in this codebase (e.g.
    # nodes/compute_nodes.hpp's MinMaxNode) in any translation unit that includes both — discovered
    # empirically building tests/drivers/opcua_driver.cpp on Windows/clang.
    target_compile_definitions(aero-thirdparty-opcua INTERFACE NOMINMAX)
  endif()
endif()

# ---- RabbitMQ (rabbitmq-c, vendored via CMake FetchContent) ----------------
# M8 (017 §6 Status): the bridge seam (`aero::broker::IBridgeSink`, broker/bridge.hpp) gets a third sink,
# `RabbitMqBridgeSink` (broker/rabbitmq_bridge_sink.hpp), over rabbitmq-c (github.com/alanxz/rabbitmq-c,
# MIT), the standard open-source C AMQP 0-9-1 client library. Mirrors `MqttBridgeSink`'s eager-connect/
# fail-fast posture (bridge.hpp's own banner) — no reconnect loop, that is a driver-specific obligation
# (006 §8), not a bridge-sink one. v1 scope: PUBLISH only, no TLS/SASL-EXTERNAL (matches this project's
# OPC-UA v1 "no security policy" precedent above), no publisher confirms, no exchange auto-declare. Kafka/
# Pulsar bridges remain backlog (M8.1/M8.2, 017 §6 Status) — out of scope here. Fetched (not vendored
# under third_party/) for the same reason as mbedTLS/open62541 above: a real C library that needs
# compiling, not just #include'ing.
option(AERO_ENABLE_RABBITMQ "Build with RabbitMQ bridge sink support (rabbitmq-c, fetched via CMake FetchContent)" ON)
if(AERO_ENABLE_RABBITMQ)
  include(FetchContent)
  FetchContent_Declare(
    rabbitmq_c
    GIT_REPOSITORY https://github.com/alanxz/rabbitmq-c.git
    GIT_TAG        v0.17.0  # latest tagged release at the time this was pinned (confirmed via git
                             # ls-remote --tags against the upstream repo)
  )
  # Keep rabbitmq-c's own build lean/quiet, mirroring the mbedTLS/open62541 forced-cache-var pattern
  # above — force the cache vars ITS OWN CMakeLists.txt reads BEFORE FetchContent_MakeAvailable:
  #   ENABLE_SSL_SUPPORT OFF — drops rabbitmq-c's own OpenSSL find_package requirement entirely: no
  #     TLS/security in this v1 (matches the OPC-UA v1 "no security policy" precedent above).
  #   BUILD_EXAMPLES / BUILD_TOOLS OFF — only the client library, never rabbitmq-c's own example
  #     programs or CLI tools as build targets.
  #   RUN_SYSTEM_TESTS OFF — rabbitmq-c's own live-broker system tests never build as a FetchContent
  #     subproject anyway; forced explicitly here for clarity, not because it would otherwise fire.
  #   BUILD_STATIC_LIBS is left at its upstream default (ON) — the static target is what gets linked,
  #     below; we don't need the shared one.
  set(ENABLE_SSL_SUPPORT OFF CACHE BOOL "" FORCE)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(RUN_SYSTEM_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(rabbitmq_c)
endif()

# aero-thirdparty-rabbitmq — INTERFACE seam over rabbitmq-c (mirrors aero-thirdparty-opcua's shape above).
# With AERO_ENABLE_RABBITMQ on, links the real static target rabbitmq-c's own CMakeLists.txt registers:
# `rabbitmq::rabbitmq-static` — and defines AERO_RABBITMQ_ENABLED=1 so
# include/aero/broker/rabbitmq_bridge_sink.hpp can compile its real rabbitmq-c-backed implementation
# instead of the "not compiled in" stub. With AERO_ENABLE_RABBITMQ off this stays a genuine no-op
# INTERFACE target: no fetch attempted, no network access needed, nothing to link.
add_library(aero-thirdparty-rabbitmq INTERFACE)
if(AERO_ENABLE_RABBITMQ)
  target_link_libraries(aero-thirdparty-rabbitmq INTERFACE rabbitmq::rabbitmq-static)
  target_compile_definitions(aero-thirdparty-rabbitmq INTERFACE AERO_RABBITMQ_ENABLED=1)
endif()

# ---- Vendored, header-only third-party deps (already fetched under third_party/) -------------
# Wired as INTERFACE targets so the one-way layering (R1) can fence the heavy httplib to aero-api /
# aero-cli only; nlohmann::json may be used more broadly (schema/core), never httplib.
add_library(aero-thirdparty-json INTERFACE)
target_include_directories(aero-thirdparty-json INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

add_library(aero-thirdparty-httplib INTERFACE)
target_include_directories(aero-thirdparty-httplib INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib")
target_link_libraries(aero-thirdparty-httplib INTERFACE Threads::Threads)

# ---- QuarkCpp dependency (the actor engine — reused, never forked) ---------
set(QUARK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../QuarkCpp" CACHE PATH "Path to the QuarkCpp checkout")
if(NOT EXISTS "${QUARK_DIR}/CMakeLists.txt")
  message(FATAL_ERROR "QuarkCpp not found at '${QUARK_DIR}'. Set -DQUARK_DIR=/path/to/QuarkCpp.")
endif()
# Build Quark lean: we only need the core library, not its tests/bench/samples.
set(QUARK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(QUARK_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(QUARK_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory("${QUARK_DIR}" "${CMAKE_BINARY_DIR}/quark")

# ---- AeroEdge libraries (header-first, mirroring Quark's posture) ----------
# aero-sdk — the stable public surface extensions compile against (INode/IDriver/context, 005/006).
add_library(aero-sdk INTERFACE)
target_include_directories(aero-sdk INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(aero-sdk INTERFACE cxx_std_23)
target_link_libraries(aero-sdk INTERFACE quark::quark)
add_library(aero::sdk ALIAS aero-sdk)

# aero-core — Flow Runtime + ProcessingContext + EdgeActorBase + registries (001–005 §5).
add_library(aero-core INTERFACE)
target_include_directories(aero-core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-core INTERFACE aero-sdk quark::quark aero-thirdparty-json)
add_library(aero::core ALIAS aero-core)

# aero-sdk-native — the C ABI a NATIVE extension author compiles against (008 §3). Header-only, pulls
# in nothing but the stable C boundary (ext_abi.h) so an out-of-tree `.so` shares no C++ type (E2).
add_library(aero-sdk-native INTERFACE)
target_include_directories(aero-sdk-native INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(aero::sdk-native ALIAS aero-sdk-native)

# aero-nodes — built-in node library (005) + the low-code ExprRule DSL (008 §6) + Phase-10 compute
# breadth (compute_nodes: mean/minmax/sum/crc, JSON-parse + Modbus-decode Sources) and the MES flow
# nodes (mes_nodes). JsonParseNode uses nlohmann::json (header-only, allowed broadly — never httplib).
add_library(aero-nodes INTERFACE)
target_include_directories(aero-nodes INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-nodes INTERFACE aero-sdk aero-thirdparty-json)
add_library(aero::nodes ALIAS aero-nodes)

# aero-mes — the MES integration hook (012): the canonical DTOs + IMesAdapter seam (mes.hpp), the
# RestMesAdapter over httplib, and the transactional OUTBOX + MesGateway actor over the Quark 012 store.
# httplib is fenced to this layer (R1). Depends on aero-core (DurableState) + Quark; sits beside
# aero-nodes/aero-drivers below aero-runtime — NOT linked by aero-core (a seam-side adapter, off hot path).
add_library(aero-mes INTERFACE)
target_include_directories(aero-mes INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-mes INTERFACE aero-core aero-sdk aero-thirdparty-httplib aero-thirdparty-json quark::quark)
add_library(aero::mes ALIAS aero-mes)

# aero-ota — Firmware OTA (011): the per-device state machine + mock OTA driver (ota.hpp) and the
# FleetActor staged-rollout policy (fleet.hpp). Durable OTA progress reuses aero-core's DurableState over
# the Quark 012 seam; the device write goes through the fenced IDriver::write (aero-sdk). No httplib.
add_library(aero-ota INTERFACE)
target_include_directories(aero-ota INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-ota INTERFACE aero-core aero-sdk quark::quark)
add_library(aero::ota ALIAS aero-ota)

# aero-ext — the HOST side of the extension model (008): the Native `.so` loader (dlopen + INode
# adapter) and the WASM runtime seam (interface + honest gate). Needs dl for dlopen (native_loader).
add_library(aero-ext INTERFACE)
target_include_directories(aero-ext INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-ext INTERFACE aero-core aero-nodes aero-thirdparty-json ${CMAKE_DL_LIBS})
add_library(aero::ext ALIAS aero-ext)

# aero-drivers — built-in driver library: the ingestion producer path over the Quark 024 stream (006).
# M9b adds aero-thirdparty-opcua: OpcUaDriver (drivers/opcua_driver.hpp) needs the real open62541-backed
# implementation (AERO_OPCUA_ENABLED + the open62541 lib) transitively wherever this library is linked
# into a real binary (aero-runtime -> aero-runtime-daemon, and this library's own tests) — same reasoning
# as aero-broker linking aero-thirdparty-tls for native_broker.hpp's TLS listener (M5).
add_library(aero-drivers INTERFACE)
target_include_directories(aero-drivers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-drivers INTERFACE aero-sdk aero-thirdparty-opcua)
add_library(aero::drivers ALIAS aero-drivers)

# aero-transport — inter-actor transport ADAPTERS behind Quark's `Transport` seam (014, X1). The FIFO/
# dedup resequencing shim (C1), the ITransport adapter base + per-link selector, and the MQTT/gRPC
# adapter shapes (offline-gated). Plugs into quark::DistributedRouter; deliberately NOT linked by
# aero-core/aero-runtime — a transport adapter is an optional backend behind the seam, off the hot path.
add_library(aero-transport INTERFACE)
target_include_directories(aero-transport INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
# Threads: the REAL socket backends (tcp/mqtt/grpc adapters) run accept/reader loops on std::thread.
target_link_libraries(aero-transport INTERFACE quark::quark Threads::Threads)
add_library(aero::transport ALIAS aero-transport)

# aero-broker — AeroEdge's own embedded MQTT server (017): local wire-protocol session state
# (CONNECT/SUBSCRIBE/PUBLISH/PUBACK/PUBREC/PUBREL/PUBCOMP/PINGREQ/DISCONNECT, QoS 0/1/2, Last Will,
# keep-alive, persistent sessions), single-node topic routing, retained messages. Shares its wire codec
# with aero-transport's MqttClientTransport (017 §9 N3). Single-node only — cross-node routing over
# Quark's DistributedRouter is deferred (017 follow-on phases).
# Linked by aero-runtime below (a daemon-lifetime subsystem exactly like aero-mes/aero-ota, not an
# optional backend behind a seam the way aero-transport is) — see aero-runtime's Runtime::configure_broker().
# bridge.hpp (017 §6, revised scope) adds bridging (MQTT + HTTP webhook sinks) and a rule engine over
# `on_publish()`, reusing aero-nodes' ExprRuleNode (008 §6) for the rule/expression side and httplib
# (RestMesAdapter's own pattern, mes/rest_mes_adapter.hpp) for the HTTP sink — hence the added deps.
# M5 (TLS+ACL milestone) adds aero-thirdparty-tls: native_broker.hpp's TLS listener (aero/pal/tls.hpp)
# needs the real mbedTLS-backed implementation (AERO_TLS_ENABLED + the mbedtls libs), not the "not
# compiled in" stub, whenever this library is actually linked into a binary.
# M8 adds aero-thirdparty-rabbitmq: rabbitmq_bridge_sink.hpp's RabbitMqBridgeSink needs the real
# rabbitmq-c-backed implementation (AERO_RABBITMQ_ENABLED + the rabbitmq-c static lib), not the "not
# compiled in" stub, whenever this library is actually linked into a binary — same reasoning as the
# TLS listener above.
add_library(aero-broker INTERFACE)
target_include_directories(aero-broker INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-broker INTERFACE
  aero-transport aero-nodes aero-thirdparty-httplib aero-thirdparty-json aero-thirdparty-tls
  aero-thirdparty-rabbitmq quark::quark Threads::Threads)
add_library(aero::broker ALIAS aero-broker)

# aero-cluster — the device-affinity placement POLICY + multi-node deployment view + fenced-migration
# helper (010). Adds ONLY policy on top of Quark's placement (025)/membership (021)/fence (012): NO hash
# ring, NO gossip, NO membership protocol. Depends on aero-core (DurableState) + aero-sdk (IDriver) + Quark.
add_library(aero-cluster INTERFACE)
target_include_directories(aero-cluster INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-cluster INTERFACE aero-core aero-sdk quark::quark)
add_library(aero::cluster ALIAS aero-cluster)

# aero-schema — the language-neutral Application/DTO contract (009 §2, 013 T3). JSON only, no runtime.
add_library(aero-schema INTERFACE)
target_include_directories(aero-schema INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-schema INTERFACE aero-thirdparty-json)
add_library(aero::schema ALIAS aero-schema)

# aero-runtime — the edge daemon core: RuntimeController + FlowActor + built-in registration (009).
# Header-first like the rest of the stack; pulls nodes/drivers/schema + Quark + Threads (std::thread).
add_library(aero-runtime INTERFACE)
target_include_directories(aero-runtime INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-runtime INTERFACE
  aero-core aero-nodes aero-drivers aero-ext aero-mes aero-ota aero-cluster aero-schema aero-broker
  quark::quark aero-thirdparty-json Threads::Threads)
add_library(aero::runtime ALIAS aero-runtime)

# aero-api — the thin REST/SSE management shell over aero-runtime (013 §5/§9). httplib lives here only.
add_library(aero-api INTERFACE)
target_include_directories(aero-api INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-api INTERFACE aero-runtime aero-thirdparty-httplib aero-thirdparty-json)
add_library(aero::api ALIAS aero-api)

# ---- Executables: the deployable daemon + the headless CLI -----------------
add_executable(aero-runtime-daemon src/runtime/aero_runtime_main.cpp)
target_link_libraries(aero-runtime-daemon PRIVATE aero-api Threads::Threads)
set_target_properties(aero-runtime-daemon PROPERTIES OUTPUT_NAME aero-runtime)

add_executable(aero-cli src/cli/aero_cli_main.cpp)
target_link_libraries(aero-cli PRIVATE aero-thirdparty-httplib aero-thirdparty-json Threads::Threads)
set_target_properties(aero-cli PROPERTIES OUTPUT_NAME aero)

# ---- Tests -----------------------------------------------------------------
option(AEROEDGE_BUILD_TESTS "Build AeroEdge tests" ON)
if(AEROEDGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---- Benchmarks (manual, not ctest gates) -----------------------------------
option(AEROEDGE_BUILD_BENCH "Build AeroEdge benchmarks (manual load-test tools, not ctest gates)" OFF)
if(AEROEDGE_BUILD_BENCH)
  add_subdirectory(bench)
endif()
