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
add_library(aero-drivers INTERFACE)
target_include_directories(aero-drivers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-drivers INTERFACE aero-sdk)
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
  aero-core aero-nodes aero-drivers aero-ext aero-schema quark::quark aero-thirdparty-json Threads::Threads)
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
