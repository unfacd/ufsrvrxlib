# install_verification.cmake — ufsrvrxlib informational install check.
#
# Run via the `install_verification` target (cmake -P). Purely informational:
# it (1) compares the library version (UFSRVRXLIB_MAJOR.MINOR.PATCH) across
# three independent sources and reports any drift, and (2) introspects the last
# origin commit for a newly added artefact and checks its presence in the
# installed copy. It never fails the build — even a detected mismatch exits 0.
#
#   Source          Origin               Extracted from
#   ------------    ------------------   --------------------------------------------
#   system install  installed library    ${INSTALL_PREFIX}/include/ufsrvrxlib/version.h
#   git             committed HEAD       git show HEAD:include/ufsrvrxlib/version.h
#   local copy      working tree         ${SOURCE_DIR}/include/ufsrvrxlib/version.h
#
# Inputs (passed as -D from add_custom_target in CMakeLists.txt):
#   INSTALL_PREFIX  — CMAKE_INSTALL_PREFIX (system install destination)
#   SOURCE_DIR      — CMAKE_SOURCE_DIR (working tree root)
#
# Invoked as:
#   add_custom_target(install_verification
#       COMMAND ${CMAKE_COMMAND}
#           -DINSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
#           -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
#           -P ${CMAKE_SOURCE_DIR}/cmake/install_verification.cmake
#       COMMENT "Verifying ufsrvrxlib version across system install, git, and local copy"
#       VERBATIM)

# ── Guards ─────────────────────────────────────────────────────────────────

if("${SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "install_verification: SOURCE_DIR not set (invoke via the 'install_verification' target)")
endif()
if("${INSTALL_PREFIX}" STREQUAL "")
    set(INSTALL_PREFIX "/usr/local")   # CMake's default install prefix
endif()

# ── Helpers ─────────────────────────────────────────────────────────────────

# parse_version(<content> <out_var>)
# Extracts "MAJOR.MINOR.PATCH" from the text of a version.h file. Falls back
# to "N/A" when the three macros are not all present.
function(parse_version content out_var)
    set(_major "")
    set(_minor "")
    set(_patch "")
    if(content MATCHES "#define[ \t]+UFSRVRXLIB_MAJOR[ \t]+([0-9]+)")
        set(_major "${CMAKE_MATCH_1}")
    endif()
    if(content MATCHES "#define[ \t]+UFSRVRXLIB_MINOR[ \t]+([0-9]+)")
        set(_minor "${CMAKE_MATCH_1}")
    endif()
    if(content MATCHES "#define[ \t]+UFSRVRXLIB_PATCH[ \t]+([0-9]+)")
        set(_patch "${CMAKE_MATCH_1}")
    endif()
    # NOTE: use a non-empty check, not `if(_major AND _minor AND _patch)` — a
    # version component may legitimately be "0", which CMake `if()` treats as
    # false (ufsrvrxlib is at 0.x). uflib's copy uses the truthiness form, which
    # only works because its MAJOR is non-zero.
    if(NOT "${_major}" STREQUAL "" AND NOT "${_minor}" STREQUAL "" AND NOT "${_patch}" STREQUAL "")
        set(${out_var} "${_major}.${_minor}.${_patch}" PARENT_SCOPE)
    else()
        set(${out_var} "N/A" PARENT_SCOPE)
    endif()
endfunction()

# report_compare(<label> <a> <b>)
# Prints OK when both sides match, DRIFT when they differ, and a neutral
# "unavailable" line when either side could not be determined (N/A).
function(report_compare label a b)
    if("${a}" STREQUAL "N/A" OR "${b}" STREQUAL "N/A")
        message(STATUS "  [  -- ] ${label}: ${a} vs ${b} (source unavailable)")
    elseif("${a}" STREQUAL "${b}")
        message(STATUS "  [  OK ] ${label}: ${a}")
    else()
        message(STATUS "  [DRIFT] ${label}: ${a} vs ${b}")
    endif()
endfunction()

# ── Collect the three versions ─────────────────────────────────────────────

# 1. system install
set(INSTALLED_VERSION "N/A")
set(_installed_header "${INSTALL_PREFIX}/include/ufsrvrxlib/version.h")
if(EXISTS "${_installed_header}")
    file(READ "${_installed_header}" _installed_content)
    parse_version("${_installed_content}" INSTALLED_VERSION)
endif()

# 2. git (committed HEAD)
set(GIT_VERSION "N/A")
find_program(_git_executable git)
if(_git_executable)
    execute_process(
        COMMAND "${_git_executable}" -C "${SOURCE_DIR}" show HEAD:include/ufsrvrxlib/version.h
        RESULT_VARIABLE _git_rc
        OUTPUT_VARIABLE _git_content
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_git_rc EQUAL 0)
        parse_version("${_git_content}" GIT_VERSION)
    endif()
endif()

# 3. local copy (working tree)
set(LOCAL_VERSION "N/A")
set(_local_header "${SOURCE_DIR}/include/ufsrvrxlib/version.h")
if(EXISTS "${_local_header}")
    file(READ "${_local_header}" _local_content)
    parse_version("${_local_content}" LOCAL_VERSION)
endif()

# ── Report ─────────────────────────────────────────────────────────────────

message(STATUS "--------------------------------------------------------------")
message(STATUS "ufsrvrxlib install_verification — version drift check (informational)")
message(STATUS "--------------------------------------------------------------")
message(STATUS "  local copy     (working tree)  : ${LOCAL_VERSION}   [${_local_header}]")
message(STATUS "  git HEAD       (committed)     : ${GIT_VERSION}")
message(STATUS "  system install (${INSTALL_PREFIX}) : ${INSTALLED_VERSION}   [${_installed_header}]")
message(STATUS "--------------------------------------------------------------")

report_compare("local  vs  git           " "${LOCAL_VERSION}" "${GIT_VERSION}")
report_compare("local  vs  system install" "${LOCAL_VERSION}" "${INSTALLED_VERSION}")
report_compare("git    vs  system install" "${GIT_VERSION}" "${INSTALLED_VERSION}")

if("${LOCAL_VERSION}" STREQUAL "N/A" OR
   "${GIT_VERSION}" STREQUAL "N/A" OR
   "${INSTALLED_VERSION}" STREQUAL "N/A")
    message(STATUS "=> comparison incomplete — one or more sources unavailable")
elseif("${LOCAL_VERSION}" STREQUAL "${GIT_VERSION}" AND
       "${LOCAL_VERSION}" STREQUAL "${INSTALLED_VERSION}")
    message(STATUS "=> all three sources agree on ${LOCAL_VERSION}")
else()
    message(STATUS "=> DRIFT detected (see lines above) — informational only, build unaffected")
endif()
message(STATUS "--------------------------------------------------------------")

# ── Introspection: newly added artefact in the last origin commit ──────────
#
# Looks up the last commit on the origin remote, finds a newly added artefact
# (a header or a .c source), and checks that it is present in the installed
# copy:
#   - header → ${INSTALL_PREFIX}/include/<path>
#   - .c     → its object member in the installed archive
# When the artefact cannot be determined — no origin remote/HEAD, or the last
# commit added no header/source — it reports "introspective verification not
# possible". Optional -DORIGIN_REF=<ref> pins the commit (default: origin HEAD).

function(introspect_origin_artefact source_dir install_prefix)
    find_program(_git git)
    find_program(_ar ar)

    set(_ref "${ORIGIN_REF}")

    # Auto-detect the origin HEAD: origin/HEAD, else origin/master, else origin/main.
    if("${_ref}" STREQUAL "" AND _git)
        foreach(_cand origin/HEAD origin/master origin/main)
            execute_process(
                COMMAND "${_git}" -C "${source_dir}" rev-parse --verify --quiet "${_cand}"
                RESULT_VARIABLE _rc
                OUTPUT_VARIABLE _sha
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            if(_rc EQUAL 0 AND NOT "${_sha}" STREQUAL "")
                set(_ref "${_cand}")
                break()
            endif()
        endforeach()
    endif()

    if("${_ref}" STREQUAL "" OR NOT _git)
        message(STATUS "  [  -- ] introspective verification not possible (no origin remote/HEAD)")
        return()
    endif()

    # Last commit subject — reported for context.
    execute_process(
        COMMAND "${_git}" -C "${source_dir}" log -1 --format=%s "${_ref}"
        OUTPUT_VARIABLE _subject
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )

    # Files ADDED by that commit.
    execute_process(
        COMMAND "${_git}" -C "${source_dir}" diff-tree --root --no-commit-id --diff-filter=A --name-only -r "${_ref}"
        OUTPUT_VARIABLE _added
        ERROR_QUIET
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    string(REGEX MATCHALL "[^\n]+" _added_files "${_added}")

    # Prefer a header; else a .c source.
    set(_artefact "")
    set(_kind "")
    foreach(_f IN LISTS _added_files)
        if(_f MATCHES "\\.h$")
            set(_artefact "${_f}")
            set(_kind "header")
            break()
        endif()
    endforeach()
    if("${_artefact}" STREQUAL "")
        foreach(_f IN LISTS _added_files)
            if(_f MATCHES "\\.c$")
                set(_artefact "${_f}")
                set(_kind "source")
                break()
            endif()
        endforeach()
    endif()

    if("${_artefact}" STREQUAL "")
        message(STATUS "  [  -- ] introspective verification not possible (last commit added no header/source)")
        message(STATUS "          last origin commit: ${_subject}")
        return()
    endif()

    message(STATUS "  last origin commit '${_subject}' added '${_artefact}'")

    if(_kind STREQUAL "header")
        string(REGEX REPLACE "^include/" "" _rel "${_artefact}")
        set(_installed "${install_prefix}/include/${_rel}")
        if(EXISTS "${_installed}")
            message(STATUS "  [  OK ] header artefact installed: ${_installed}")
        else()
            message(STATUS "  [MISS ] header artefact NOT installed (expected ${_installed})")
        endif()
    else()
        # .c source → its object member must be in the installed archive.
        get_filename_component(_base "${_artefact}" NAME)
        set(_found FALSE)
        foreach(_libname libufsrvrxlib.a libufsrvrxlib.so)
            set(_lib "${install_prefix}/lib/${_libname}")
            if(EXISTS "${_lib}" AND _ar)
                execute_process(
                    COMMAND "${_ar}" t "${_lib}"
                    OUTPUT_VARIABLE _members
                    ERROR_QUIET
                )
                string(FIND "${_members}" "${_base}.o" _pos)
                if(_pos GREATER -1)
                    message(STATUS "  [  OK ] source artefact compiled into ${_lib} (${_base}.o)")
                    set(_found TRUE)
                    break()
                endif()
            endif()
        endforeach()
        if(NOT _found)
            message(STATUS "  [MISS ] source artefact not found in installed library (${_artefact})")
        endif()
    endif()
endfunction()

message(STATUS "--------------------------------------------------------------")
message(STATUS "ufsrvrxlib install_verification — origin artefact introspection")
message(STATUS "--------------------------------------------------------------")
introspect_origin_artefact("${SOURCE_DIR}" "${INSTALL_PREFIX}")
message(STATUS "--------------------------------------------------------------")
