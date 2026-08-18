# FetchContent PATCH_COMMAND for open62541 (see root CMakeLists.txt's AERO_ENABLE_OPCUA block). One
# content-matched (not line-numbered) fix. PATCH_COMMAND runs with the freshly-fetched open62541 source
# directory as the working directory. Idempotent — CMake's FetchContent re-runs PATCH_COMMAND on every
# reconfigure that touches the FetchContent_Declare() call site, not just the first populate, and does
# not track "already patched" itself — a second run against an already-patched checkout must be a silent
# no-op, not a FATAL_ERROR, or every reconfigure after the first would break the build.

# ============================================================================================
# Patch 1 — drop open62541's install/export rules for the `open62541` target.
#
# M9.4 (018 §Security policies) turns on UA_ENABLE_ENCRYPTION=MBEDTLS, which makes open62541 link this
# project's own vendored mbedtls/mbedx509/mbedcrypto targets (see root CMakeLists.txt's AERO_ENABLE_TLS
# block above the OPC-UA one — deliberately the SAME build, not a second vendored copy, to avoid linking
# two independent static libs that export identical C symbols). open62541's own CMakeLists.txt
# unconditionally declares `install(TARGETS open62541 EXPORT open62541Targets ...)` +
# `install(EXPORT open62541Targets ...)` + a build-tree `export(TARGETS open62541 ...)` — CMake validates
# at GENERATE time (not just `cmake --install` time) that every PUBLIC/INTERFACE-linked dependency of an
# exported target is itself part of an export set. AeroEdge never installs or exports mbedTLS (it isn't
# an installed package here, just an in-tree FetchContent build target), so once open62541 links it, that
# validation fails ("export called with target 'open62541' which requires target 'mbedtls' that is not
# in any export set") — confirmed empirically. AeroEdge has no install() rules of its own and never runs
# `cmake --install` on this tree, and nothing here consumes an installed/exported open62541 package (this
# project links the in-tree `open62541` target directly, never via `find_package(open62541)`), so these
# rules serve no purpose and are dropped rather than worked around.
# ============================================================================================
set(_f1 "CMakeLists.txt")
file(READ "${_f1}" _content1)

set(_search1a "install(TARGETS open62541
        EXPORT open62541Targets
        LIBRARY DESTINATION \${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION \${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION \${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION include)")
set(_replace1a "# AeroEdge patch (cmake/patch_open62541.cmake): install(EXPORT) disabled below, so this
# target's EXPORT association is dropped too -- this project never runs `cmake --install` on this tree.
install(TARGETS open62541
        LIBRARY DESTINATION \${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION \${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION \${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION include)")

set(_search1b "install(EXPORT open62541Targets
        FILE open62541Targets.cmake
        DESTINATION \"\${cmake_configfile_install}\"
        NAMESPACE open62541::)
export(TARGETS open62541
       NAMESPACE open62541::
       FILE \${CMAKE_CURRENT_BINARY_DIR}/open62541Targets.cmake)")
set(_replace1b "# AeroEdge patch (cmake/patch_open62541.cmake): install(EXPORT)/export(TARGETS) disabled --
# would require mbedtls/mbedx509/mbedcrypto (linked in for UA_ENABLE_ENCRYPTION=MBEDTLS) to also be
# exported, which this project's own FetchContent'd mbedTLS build never is. Not needed: AeroEdge links
# the in-tree `open62541` target directly, never via an installed/exported package.
# install(EXPORT open62541Targets FILE open62541Targets.cmake DESTINATION \"\${cmake_configfile_install}\" NAMESPACE open62541::)
# export(TARGETS open62541 NAMESPACE open62541:: FILE \${CMAKE_CURRENT_BINARY_DIR}/open62541Targets.cmake)")

string(FIND "${_content1}" "${_replace1a}" _already_patched1)
if(_already_patched1 EQUAL -1)
  string(FIND "${_content1}" "${_search1a}" _pos1a)
  string(FIND "${_content1}" "${_search1b}" _pos1b)
  if(_pos1a EQUAL -1 OR _pos1b EQUAL -1)
    message(FATAL_ERROR "patch_open62541.cmake (patch 1): expected text not found in ${_f1} -- "
                         "open62541's CMakeLists.txt layout changed; update this patch or the pinned "
                         "GIT_TAG.")
  endif()
  string(REPLACE "${_search1a}" "${_replace1a}" _content1 "${_content1}")
  string(REPLACE "${_search1b}" "${_replace1b}" _content1 "${_content1}")
  file(WRITE "${_f1}" "${_content1}")
endif()

# ============================================================================================
# Patch 2 — drop the mbedtls_entropy_self_test() call from each mbedTLS-backed SecurityPolicy.
#
# cmake/patch_mbedtls.cmake's patch 2 disables MBEDTLS_SELF_TEST project-wide (a real, TSan-confirmed
# unsynchronized-global-counter data race under concurrent TLS handshakes, in code this project's own
# TLS layer never calls anyway). With MBEDTLS_SELF_TEST off, `mbedtls_entropy_self_test()`'s declaration
# disappears from mbedTLS's own headers — but every open62541 SecurityPolicy implementation
# (aes256sha256rsapss/basic128rsa15/basic256sha256/aes128sha256rsaoaep/basic256) calls it unconditionally,
# once, during that policy's one-time context setup (UA_SecurityPolicy_..._New), gating success on its
# result — not a per-handshake hot path, so this is unrelated to the ecp.c race patch 2 fixes. Modern
# Clang treats an implicit function declaration as a hard C error (not a warning), so this must be
# resolved, not ignored. Rather than re-enabling MBEDTLS_SELF_TEST (which would reintroduce the real TSan
# race for the sake of a one-time RNG self-check that isn't this project's threading concern), each call
# site is patched to skip the self-test (mbedErr = 0, i.e. "assume pass") — the policy's actual DRBG seed
# call right after this still runs and still fails loudly (UA_STATUSCODE_BADSECURITYCHECKSFAILED) if the
# entropy source is genuinely broken.
# ============================================================================================
set(_f2_list
  "plugins/crypto/mbedtls/ua_securitypolicy_aes256sha256rsapss.c"
  "plugins/crypto/mbedtls/ua_securitypolicy_basic128rsa15.c"
  "plugins/crypto/mbedtls/ua_securitypolicy_basic256sha256.c"
  "plugins/crypto/mbedtls/ua_securitypolicy_aes128sha256rsaoaep.c"
  "plugins/crypto/mbedtls/ua_securitypolicy_basic256.c"
)

set(_search2 "    mbedErr = mbedtls_entropy_self_test(0);
")
set(_replace2 "    /* AeroEdge patch (cmake/patch_open62541.cmake, patch 2): MBEDTLS_SELF_TEST is disabled
     * project-wide (see cmake/patch_mbedtls.cmake, patch 2 -- a real TSan-confirmed race elsewhere in
     * mbedTLS's self-test instrumentation), which removes this function's declaration. Skip the
     * one-time entropy self-test rather than reintroducing that race; the DRBG seed call right below
     * still fails loudly if the entropy source is genuinely broken. */
    mbedErr = 0;
")

foreach(_f2 IN LISTS _f2_list)
  file(READ "${_f2}" _content2)
  string(FIND "${_content2}" "${_replace2}" _already_patched2)
  if(_already_patched2 EQUAL -1)
    string(FIND "${_content2}" "${_search2}" _pos2)
    if(_pos2 EQUAL -1)
      message(FATAL_ERROR "patch_open62541.cmake (patch 2): expected text not found in ${_f2} -- "
                           "open62541's SecurityPolicy layout changed; update this patch or the pinned "
                           "GIT_TAG.")
    endif()
    string(REPLACE "${_search2}" "${_replace2}" _content2 "${_content2}")
    file(WRITE "${_f2}" "${_content2}")
  endif()
endforeach()
