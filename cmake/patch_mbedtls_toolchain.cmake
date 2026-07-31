# FetchContent PATCH_COMMAND for mbedTLS (see root CMakeLists.txt's AERO_ENABLE_TLS block).
#
# WHY THIS EXISTS: this project's Windows leg of its two-compiler gate runs Clang via its GNU-style
# driver (`clang++.exe`, NOT `clang-cl.exe`) while targeting the MSVC ABI. In that configuration CMake's
# compiler-ABI detection genuinely sets CMAKE_C_SIMULATE_ID=MSVC (Clang really does simulate the MSVC
# ABI there) — but mbedTLS's own root CMakeLists.txt (v3.6.x) treats a SIMULATE_ID of "MSVC" as "this is
# cl.exe" and appends cl-only flags (`/W3 /utf-8`) to CMAKE_C_FLAGS, which a GNU-style driver's
# command-line parser rejects outright ("no such file or directory: '/W3'"). The ABI and the accepted
# command-line SYNTAX are two different things Clang-on-Windows can disagree about; mbedTLS's check only
# looks at the former. This is a one-line, content-matched patch (not a line-number patch, since content
# match survives any incidental reformatting between mbedTLS patch releases within the pinned v3.6.x
# line) that makes mbedTLS classify the compiler from CMAKE_C_COMPILER_ID (yields "Clang", correct)
# instead of CMAKE_C_SIMULATE_ID (yields "MSVC", a false positive for this specific toolchain shape).
#
# PATCH_COMMAND runs with the freshly-fetched mbedTLS source directory as the working directory.
set(_f "CMakeLists.txt")
file(READ "${_f}" _content)

set(_search "    set(COMPILER_ID \${CMAKE_C_SIMULATE_ID})")
set(_replace "    set(COMPILER_ID \${CMAKE_C_COMPILER_ID})  # AeroEdge patch: SIMULATE_ID misfires MSVC for Clang/GNU-driver+MSVC-ABI")

# Idempotency: CMake's FetchContent re-runs PATCH_COMMAND on every reconfigure that touches the
# FetchContent_Declare() call site (e.g. an unrelated edit elsewhere in the top-level CMakeLists.txt),
# not just on the very first populate — it does NOT track "already patched" itself. A second run against
# an already-patched checkout must be a silent no-op, not a FATAL_ERROR, or every reconfigure after the
# first would break the build.
string(FIND "${_content}" "${_replace}" _already_patched)
if(NOT _already_patched EQUAL -1)
  return()
endif()

string(FIND "${_content}" "${_search}" _pos)
if(_pos EQUAL -1)
  message(FATAL_ERROR "patch_mbedtls_toolchain.cmake: expected text not found in ${_f} — mbedTLS's "
                       "CMakeLists.txt layout changed; update this patch or the pinned GIT_TAG.")
endif()

string(REPLACE "${_search}" "${_replace}" _content "${_content}")
file(WRITE "${_f}" "${_content}")
