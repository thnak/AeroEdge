# FetchContent PATCH_COMMAND for mbedTLS (see root CMakeLists.txt's AERO_ENABLE_TLS block). Two
# independent, content-matched (not line-numbered, so a patch-release bump within the pinned v3.6.x
# line doesn't silently stop applying) fixes for real problems this project's build hits that a
# default mbedTLS checkout doesn't anticipate. PATCH_COMMAND runs with the freshly-fetched mbedTLS
# source directory as the working directory. Both patches are individually idempotent (CMake's
# FetchContent re-runs PATCH_COMMAND on every reconfigure that touches the FetchContent_Declare()
# call site, not just the first populate, and does not track "already patched" itself) — a second run
# against an already-patched checkout is a silent no-op for whichever patch already applied, not a
# FATAL_ERROR, or every reconfigure after the first would break the build.

# ============================================================================================
# Patch 1 — toolchain misdetection (root CMakeLists.txt).
#
# This project's Windows leg of its two-compiler gate runs Clang via its GNU-style driver
# (`clang++.exe`, NOT `clang-cl.exe`) while targeting the MSVC ABI. In that configuration CMake's
# compiler-ABI detection genuinely sets CMAKE_C_SIMULATE_ID=MSVC (Clang really does simulate the MSVC
# ABI there) — but mbedTLS's own root CMakeLists.txt (v3.6.x) treats a SIMULATE_ID of "MSVC" as "this
# is cl.exe" and appends cl-only flags (`/W3 /utf-8`) to CMAKE_C_FLAGS, which a GNU-style driver's
# command-line parser rejects outright ("no such file or directory: '/W3'"). The ABI and the accepted
# command-line SYNTAX are two different things Clang-on-Windows can disagree about; mbedTLS's check
# only looks at the former. Makes mbedTLS classify the compiler from CMAKE_C_COMPILER_ID (yields
# "Clang", correct) instead of CMAKE_C_SIMULATE_ID (yields "MSVC", a false positive here).
# ============================================================================================
set(_f1 "CMakeLists.txt")
file(READ "${_f1}" _content1)

set(_search1 "    set(COMPILER_ID \${CMAKE_C_SIMULATE_ID})")
set(_replace1 "    set(COMPILER_ID \${CMAKE_C_COMPILER_ID})  # AeroEdge patch: SIMULATE_ID misfires MSVC for Clang/GNU-driver+MSVC-ABI")

string(FIND "${_content1}" "${_replace1}" _already_patched1)
if(_already_patched1 EQUAL -1)
  string(FIND "${_content1}" "${_search1}" _pos1)
  if(_pos1 EQUAL -1)
    message(FATAL_ERROR "patch_mbedtls.cmake (patch 1): expected text not found in ${_f1} — mbedTLS's "
                         "CMakeLists.txt layout changed; update this patch or the pinned GIT_TAG.")
  endif()
  string(REPLACE "${_search1}" "${_replace1}" _content1 "${_content1}")
  file(WRITE "${_f1}" "${_content1}")
endif()

# ============================================================================================
# Patch 2 — disable MBEDTLS_SELF_TEST (default config header).
#
# mbedTLS's default `mbedtls_config.h` unconditionally `#define`s MBEDTLS_SELF_TEST, which compiles
# in each module's `mbedtls_xxx_self_test()` — but ALSO compiles in `ecp.c`'s `INC_MUL_COUNT` macro
# (`mul_count++` on a plain `static unsigned long`, gated on this exact macro, ecp.c:1040-1043),
# unconditionally hit inside `mbedtls_mpi_mul_mod` on the hot EC-crypto path every TLS handshake
# takes. That counter exists purely to support `mbedtls_ecp_self_test`'s benchmark output — nothing
# in this codebase calls it (we already build with ENABLE_TESTING/ENABLE_PROGRAMS OFF, so none of
# mbedTLS's own self-test binaries are even built) — but it is a genuine, unsynchronized global write
# from every concurrent handshake thread, which ThreadSanitizer correctly flags as a data race
# (`native_broker_security`/`tls_channel`/`broker_cluster` under the `thread` sanitizer CI leg,
# confirmed via the exact stack: mbedtls_mpi_mul_mod -> ecp_double_jac/ecp_normalize_jac -> ... ->
# two concurrent TLS handshake threads). It is a benign correctness-wise race (the counter's value is
# never read outside the self-test routine this build never calls) but ThreadSanitizer cannot know
# that, and a real race — however benign — inside a dependency is still a hard TSan-gate failure this
# project's CI treats as blocking (CONVENTIONS.md). Root-causing beats suppressing: since the counter
# only exists behind MBEDTLS_SELF_TEST and we build no self-test binaries, disabling the macro at its
# single unconditional #define removes the race entirely rather than papering over it with a
# suppression list.
# ============================================================================================
set(_f2 "include/mbedtls/mbedtls_config.h")
file(READ "${_f2}" _content2)

set(_search2 "#define MBEDTLS_SELF_TEST")
set(_replace2 "// #undef MBEDTLS_SELF_TEST -- AeroEdge patch: unused self-test hooks include an unsynchronized global counter (ecp.c mul_count) that TSan flags as a real data race under concurrent TLS handshakes; this build never calls mbedtls_*_self_test()")

string(FIND "${_content2}" "${_replace2}" _already_patched2)
if(_already_patched2 EQUAL -1)
  string(FIND "${_content2}" "${_search2}" _pos2)
  if(_pos2 EQUAL -1)
    message(FATAL_ERROR "patch_mbedtls.cmake (patch 2): expected text not found in ${_f2} — mbedTLS's "
                         "default config layout changed; update this patch or the pinned GIT_TAG.")
  endif()
  string(REPLACE "${_search2}" "${_replace2}" _content2 "${_content2}")
  file(WRITE "${_f2}" "${_content2}")
endif()

# ============================================================================================
# Patch 3 — enable MBEDTLS_THREADING_C/MBEDTLS_THREADING_ALT (default config header) + drop in
# `include/threading_alt.h` (mbedTLS's own include root, already on its public include path).
#
# TLS 1.3 in mbedTLS 3.6 routes its handshake crypto through the PSA subsystem (psa_crypto*.c),
# which holds process-wide global mutable state (`global_data`, the PSA key-slot table) that is
# NOT synchronized unless mbedTLS's own threading abstraction is enabled — mbedTLS's own config
# comment for MBEDTLS_THREADING_C says exactly this: "you must enable it... even if individual TLS
# contexts are not shared between threads." NativeBroker legitimately runs one TLS handshake per
# accepted connection, each on its own thread, sharing one process — precisely the scenario this
# guards. Confirmed via TSan: concurrent `psa_allocate_volatile_key_slot`/`psa_free_key_slot` calls
# from different handshake threads race on the same key-slot table (psa_crypto_slot_management.c).
#
# MBEDTLS_THREADING_ALT (not _PTHREAD) is the portable choice: this project's Windows leg has no
# guaranteed pthread.h (Clang's GNU-style driver targeting the MSVC ABI, not MinGW), while
# std::mutex is uniformly available on every platform this project targets. `threading_alt.h` only
# declares the mbedtls_threading_mutex_t STORAGE SHAPE (an opaque pointer) — the actual std::mutex-
# backed init/free/lock/unlock functions and the one required mbedtls_threading_set_alt() call live
# in include/aero/pal/tls.hpp (C++ code, run once via std::call_once before any other mbedTLS call,
# from TlsServerContext::create()), not here — this file only makes the TYPE mbedTLS's own C
# translation units compile against match what that C++ shim expects.
# ============================================================================================
set(_f3 "include/mbedtls/mbedtls_config.h")
file(READ "${_f3}" _content3)

set(_search3a "//#define MBEDTLS_THREADING_ALT")
set(_replace3a "#define MBEDTLS_THREADING_ALT  // AeroEdge patch: PSA global key-slot table needs synchronization across concurrent TLS handshake threads")
set(_search3b "//#define MBEDTLS_THREADING_C")
set(_replace3b "#define MBEDTLS_THREADING_C  // AeroEdge patch: required by MBEDTLS_THREADING_ALT above")

string(FIND "${_content3}" "${_replace3a}" _already_patched3)
if(_already_patched3 EQUAL -1)
  string(FIND "${_content3}" "${_search3a}" _pos3a)
  string(FIND "${_content3}" "${_search3b}" _pos3b)
  if(_pos3a EQUAL -1 OR _pos3b EQUAL -1)
    message(FATAL_ERROR "patch_mbedtls.cmake (patch 3): expected text not found in ${_f3} — mbedTLS's "
                         "default config layout changed; update this patch or the pinned GIT_TAG.")
  endif()
  string(REPLACE "${_search3a}" "${_replace3a}" _content3 "${_content3}")
  string(REPLACE "${_search3b}" "${_replace3b}" _content3 "${_content3}")
  file(WRITE "${_f3}" "${_content3}")
endif()

set(_alt_header "include/threading_alt.h")
if(NOT EXISTS "${_alt_header}")
  file(WRITE "${_alt_header}" "\
/* AeroEdge patch (cmake/patch_mbedtls.cmake, patch 3): the storage shape mbedtls_threading_mutex_t\n\
 * must have under MBEDTLS_THREADING_ALT. Deliberately opaque (one pointer) -- the real mutex is a\n\
 * std::mutex, heap-allocated and managed from C++ (include/aero/pal/tls.hpp); this header only has\n\
 * to satisfy mbedTLS's own C translation units, which never dereference the field themselves. */\n\
#ifndef AERO_MBEDTLS_THREADING_ALT_H\n\
#define AERO_MBEDTLS_THREADING_ALT_H\n\
\n\
#ifdef __cplusplus\n\
extern \"C\" {\n\
#endif\n\
\n\
typedef struct mbedtls_threading_mutex_t {\n\
    void *aero_native_mutex; /* owned std::mutex*, see aero::pal::tls's threading shim */\n\
} mbedtls_threading_mutex_t;\n\
\n\
#ifdef __cplusplus\n\
}\n\
#endif\n\
\n\
#endif /* AERO_MBEDTLS_THREADING_ALT_H */\n\
")
endif()
