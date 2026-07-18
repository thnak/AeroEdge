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

# aero-nodes — built-in node library (005).
add_library(aero-nodes INTERFACE)
target_include_directories(aero-nodes INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-nodes INTERFACE aero-sdk)
add_library(aero::nodes ALIAS aero-nodes)

# aero-drivers — built-in driver library: the ingestion producer path over the Quark 024 stream (006).
add_library(aero-drivers INTERFACE)
target_include_directories(aero-drivers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-drivers INTERFACE aero-sdk)
add_library(aero::drivers ALIAS aero-drivers)

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
  aero-core aero-nodes aero-drivers aero-schema quark::quark aero-thirdparty-json Threads::Threads)
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
