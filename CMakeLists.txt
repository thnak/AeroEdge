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

# aero-core — Flow Runtime + ProcessingContext + EdgeActorBase (001–004).
add_library(aero-core INTERFACE)
target_include_directories(aero-core INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-core INTERFACE aero-sdk quark::quark)
add_library(aero::core ALIAS aero-core)

# aero-nodes — built-in node library (005).
add_library(aero-nodes INTERFACE)
target_include_directories(aero-nodes INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(aero-nodes INTERFACE aero-sdk)
add_library(aero::nodes ALIAS aero-nodes)

# ---- Tests -----------------------------------------------------------------
option(AEROEDGE_BUILD_TESTS "Build AeroEdge tests" ON)
if(AEROEDGE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
