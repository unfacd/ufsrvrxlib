# cmake-modules/ufsrv_build_options.cmake
#
# SHARED build-options module for the ufsrv repo family (companion libraries AND
# server applications). Authored to be byte-identical across every repo.
#
# Contract:
#   * include() this AFTER  project(<name> LANGUAGES C [CXX] ...)
#     and BEFORE creating any target (it reads CMAKE_C_COMPILER_ID).
#   * Optionally set the UFSRV_C_WARNING_FLAGS list BEFORE include() to choose
#     THIS repo's warning policy — warning flags are per-repo; everything else in
#     this module is shared/identical. If unset, only -Wall is applied.
#   * Call  ufsrv_apply_build_options(<tgt>)  on EVERY target you create (OBJECT
#     libs, STATIC rollups, executables, gtest test executables) so both the
#     fixed compile flags AND (for executables) the sanitizer link flags land.
#
# Invocation collapses to a preset, e.g.:
#   cmake --preset asan && cmake --build --preset asan

include_guard(GLOBAL)

# ── 1. Enforce clang (the code uses Apple Blocks: -fblocks) ──────────────────
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "ufsrv requires the Clang C compiler (the code uses Apple Blocks / -fblocks).\n"
        "  Detected: '${CMAKE_C_COMPILER_ID}' (${CMAKE_C_COMPILER})\n"
        "  Configure with a ufsrv preset, e.g.  cmake --preset debug\n"
        "  or pass  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++.")
endif()
if(CMAKE_CXX_COMPILER_LOADED AND NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
        "ufsrv requires clang++ for C++ (gtest) translation units; "
        "detected '${CMAKE_CXX_COMPILER_ID}'.")
endif()

# ── 2. C language baseline ───────────────────────────────────────────────────
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)           # -std=gnu17 (GNU extensions used across the codebase)

# ── 2b. Per-repo debug-info scope for the Debug configuration ────────────────
# The Debug config (used by the debug/asan/msan/tsan presets) compiles with these
# debug-info flags. Pick THIS repo's default via UFSRV_C_DEBUG_FLAGS_DEFAULT before
# include(); a preset can override UFSRV_C_DEBUG_FLAGS per profile (e.g. tone it
# down to -g1 for the sanitizer presets). -O0 is clang's default when no -O given.
if(NOT DEFINED UFSRV_C_DEBUG_FLAGS_DEFAULT)
    set(UFSRV_C_DEBUG_FLAGS_DEFAULT "-g")
endif()
set(UFSRV_C_DEBUG_FLAGS "${UFSRV_C_DEBUG_FLAGS_DEFAULT}" CACHE STRING
    "Debug-info flags applied in the Debug configuration (per-repo default; preset-overridable)")
set(CMAKE_C_FLAGS_DEBUG "${UFSRV_C_DEBUG_FLAGS}")   # replaces CMake's built-in Debug '-g'

# ── 3. First-class, shared sanitizer + coverage options ─────────────────────
# Primary sanitizers — MUTUALLY EXCLUSIVE (address/memory/thread cannot combine).
option(UFSRV_BUILD_WITH_ASAN     "Build with AddressSanitizer instrumentation" OFF)
option(UFSRV_BUILD_WITH_MSAN     "Build with MemorySanitizer instrumentation"  OFF)
option(UFSRV_BUILD_WITH_TSAN     "Build with ThreadSanitizer instrumentation"  OFF)
# Additive sanitizers — combine with any primary (or stand alone).
option(UFSRV_BUILD_WITH_UBSAN    "Build with UndefinedBehaviorSanitizer"       OFF)
option(UFSRV_BUILD_WITH_LSAN     "Build with LeakSanitizer (implied by ASan)"  OFF)
option(UFSRV_BUILD_WITH_COVERAGE "Build with gcov/llvm source coverage"        OFF)

set(_ufsrv_primary_count 0)
foreach(_s ASAN MSAN TSAN)
    if(UFSRV_BUILD_WITH_${_s})
        math(EXPR _ufsrv_primary_count "${_ufsrv_primary_count}+1")
    endif()
endforeach()
if(_ufsrv_primary_count GREATER 1)
    message(FATAL_ERROR
        "Enable at most ONE primary sanitizer (UFSRV_BUILD_WITH_{ASAN,MSAN,TSAN}) — "
        "address/memory/thread are mutually exclusive. UBSan/LSan combine with any of them.")
endif()

# ── 4. Universal C flags (shared) + per-repo warning flags ──────────────────
set(_ufsrv_universal_c   -fblocks)          # Apple Blocks are mandatory across ufsrv
set(_ufsrv_universal_def _GNU_SOURCE)
if(NOT DEFINED UFSRV_C_WARNING_FLAGS)
    set(UFSRV_C_WARNING_FLAGS -Wall)        # default if a repo sets no policy of its own
endif()

# ── 5. Sanitizer flags — accumulate all enabled sanitizers into one -fsanitize= ─
# One primary (address/memory/thread) plus optional additive UBSan/LSan.
set(_ufsrv_ignorelist "${CMAKE_CURRENT_LIST_DIR}/sanitizer_ignorelist.clang")
set(_fs "")               # -fsanitize= check names
set(_san_extra "")        # extra sanitizer flags (compile + link)
set(_san_compile_only "")
set(_san_link_only "")
set(_ufsrv_active_sanitizers "")   # human-readable, for the build digest

if(UFSRV_BUILD_WITH_ASAN)
    list(APPEND _fs address signed-integer-overflow integer-divide-by-zero null bounds alignment unreachable)
    list(APPEND _ufsrv_active_sanitizers "ASan")
elseif(UFSRV_BUILD_WITH_MSAN)
    list(APPEND _fs memory)
    list(APPEND _san_extra -fstack-protector-all -fsanitize-memory-track-origins=2 -fsanitize-recover=memory)
    list(APPEND _san_compile_only -fPIE)
    list(APPEND _san_link_only -pie)
    list(APPEND _ufsrv_active_sanitizers "MSan")
elseif(UFSRV_BUILD_WITH_TSAN)
    list(APPEND _fs thread)
    list(APPEND _ufsrv_active_sanitizers "TSan")
endif()
if(UFSRV_BUILD_WITH_UBSAN)
    list(APPEND _fs undefined)
    list(APPEND _ufsrv_active_sanitizers "UBSan")
endif()
if(UFSRV_BUILD_WITH_LSAN)
    list(APPEND _fs leak)
    list(APPEND _ufsrv_active_sanitizers "LSan")
endif()

set(_san_core "")
if(_fs)
    list(JOIN _fs "," _fs_csv)
    set(_san_core -fno-omit-frame-pointer -fsanitize=${_fs_csv} ${_san_extra})
    list(APPEND _san_compile_only -fsanitize-ignorelist=${_ufsrv_ignorelist})
endif()

set(_cov_flags "")
if(UFSRV_BUILD_WITH_COVERAGE)
    set(_cov_flags -fprofile-arcs -ftest-coverage)
endif()

# ── 6. The single INTERFACE target every target consumes ─────────────────────
add_library(ufsrv_build_options INTERFACE)
add_library(ufsrv::build_options ALIAS ufsrv_build_options)

# C-language-guard the base + per-repo warning flags so C++ (gtest) TUs are
# unaffected by C-only flags (e.g. -include stdatomic.h, C warning names).
set(_c_guarded "")
foreach(_f IN LISTS _ufsrv_universal_c UFSRV_C_WARNING_FLAGS)
    list(APPEND _c_guarded "$<$<COMPILE_LANGUAGE:C>:${_f}>")
endforeach()

target_compile_definitions(ufsrv_build_options INTERFACE ${_ufsrv_universal_def})
target_compile_options(ufsrv_build_options INTERFACE
    ${_c_guarded}
    "SHELL:$<$<COMPILE_LANGUAGE:C>:-include stdatomic.h>"   # keep the two tokens adjacent
    ${_san_core} ${_san_compile_only} ${_cov_flags})

# Sanitizer + coverage flags must ALSO be on the link line of executables/tests.
target_link_options(ufsrv_build_options INTERFACE
    ${_san_core} ${_san_link_only} ${_cov_flags})

# ── 7. Apply helper — call on EVERY target ───────────────────────────────────
# Build-only link ($<BUILD_INTERFACE:>) so these flags never leak into an
# install(EXPORT) set — the exported target must not depend on this build target.
function(ufsrv_apply_build_options tgt)
    target_link_libraries(${tgt} PRIVATE $<BUILD_INTERFACE:ufsrv_build_options>)
endfunction()

# ── 8. Build digest — echoed at the END of the build (POST_BUILD) ────────────
# Call ufsrv_build_summary(<final target>) once (on the exe / static lib) to
# print which sanitizer / debug / warning options the artefact was actually
# built with — confirms the active configuration at a glance.
if(_ufsrv_active_sanitizers)
    list(JOIN _ufsrv_active_sanitizers "+" _ufsrv_san_summary)
else()
    set(_ufsrv_san_summary "none")
endif()
if(CMAKE_BUILD_TYPE)
    set(_ufsrv_bt "${CMAKE_BUILD_TYPE}")
else()
    set(_ufsrv_bt "default")
endif()
string(JOIN " " _ufsrv_warn_summary ${UFSRV_C_WARNING_FLAGS})
set(UFSRV_BUILD_DIGEST
    "[ufsrv] built with:  type=${_ufsrv_bt}  compiler=clang  sanitizers=${_ufsrv_san_summary}  coverage=${UFSRV_BUILD_WITH_COVERAGE}  debug=[${UFSRV_C_DEBUG_FLAGS}]  warnings=[${_ufsrv_warn_summary}]")

# Surface it at configure time too — this is the line IDEs (CLion, …) reliably
# show in their CMake output pane whenever a profile is (re)configured.
message(STATUS "${UFSRV_BUILD_DIGEST}")

function(ufsrv_build_summary tgt)
    # A dedicated ALL target (always out of date) prints the digest on every
    # build of the default/all target, ordered after ${tgt}. More reliable than
    # a POST_BUILD custom command, which OBJECT libraries ignore and which never
    # fires when the target is already up to date.
    if(NOT TARGET ufsrv_build_digest)
        add_custom_target(ufsrv_build_digest ALL
            COMMAND ${CMAKE_COMMAND} -E echo "${UFSRV_BUILD_DIGEST}"
            VERBATIM)
    endif()
    add_dependencies(ufsrv_build_digest ${tgt})
endfunction()

# ── 9. Preset guardrail — no preset may enable more than one primary sanitizer ─
# Runtime: the *active* resolved config is already rejected in §3.  Static: at
# configure time, validate EVERY preset in this repo's CMakePresets.json (the
# check resolves include/inherits) so a mis-authored preset is caught up front.
option(UFSRV_VALIDATE_PRESETS "Validate CMakePresets sanitizer mutual-exclusivity" ON)
find_program(UFSRV_PYTHON3 NAMES python3 python)
if(UFSRV_VALIDATE_PRESETS AND UFSRV_PYTHON3 AND EXISTS "${CMAKE_SOURCE_DIR}/CMakePresets.json")
    execute_process(
        COMMAND "${UFSRV_PYTHON3}" "${CMAKE_CURRENT_LIST_DIR}/check_preset_exclusivity.py"
                "${CMAKE_SOURCE_DIR}/CMakePresets.json"
        RESULT_VARIABLE _ufsrv_presets_rc
        OUTPUT_VARIABLE _ufsrv_presets_msg
        ERROR_VARIABLE  _ufsrv_presets_msg)
    if(NOT _ufsrv_presets_rc EQUAL 0)
        message(FATAL_ERROR "ufsrv preset guardrail — ${_ufsrv_presets_msg}")
    endif()
    if(NOT TARGET check-presets)          # on-demand / CI entry point
        add_custom_target(check-presets
            COMMAND "${UFSRV_PYTHON3}" "${CMAKE_CURRENT_LIST_DIR}/check_preset_exclusivity.py"
                    "${CMAKE_SOURCE_DIR}/CMakePresets.json"
            COMMENT "Validating CMakePresets sanitizer mutual-exclusivity"
            VERBATIM)
    endif()
endif()
