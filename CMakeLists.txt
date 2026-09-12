# ufsrvrxlib — ufsrv reactive-extensions library (coroutines, futures, scheduler)
#
# Dual-mode (mirrors the uflib / ufnetcorelib pattern):
#   IS_ROOT_PROJECT=ON  → STATIC library + install + pkg-config + CPack
#                          (+ SHARED if BUILD_SHARED_LIBS=ON)
#   IS_ROOT_PROJECT=OFF → OBJECT library (compiled into parent via add_subdirectory)
#
# Depends on the system-installed uflib (bottom layer) via find_package(uflib).
#
# Usage:
#   cmake --preset dev
#   cmake --build build/dev -j$(nproc)
#   cmake --install build/dev --prefix /usr/local

cmake_minimum_required(VERSION 3.20)

set(_PROJECT_NAME "ufsrvrxlib")
string(TOUPPER ${_PROJECT_NAME} _PROJECT_NAME_X)

# ── Version extraction ──────────────────────────────────────────────────────

macro(getVersionPatchLevel name)
    set(VERSION_REGEX "^#define ${name} (.+)$")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/ufsrvrxlib/version.h"
         VERSION_BIT REGEX ${VERSION_REGEX})
    string(REGEX REPLACE ${VERSION_REGEX} "\\1" ${name} "${VERSION_BIT}")
endmacro()

getVersionPatchLevel(${_PROJECT_NAME_X}_MAJOR)
getVersionPatchLevel(${_PROJECT_NAME_X}_MINOR)
getVersionPatchLevel(${_PROJECT_NAME_X}_PATCH)
set(${_PROJECT_NAME_X}_VERSION "${${_PROJECT_NAME_X}_MAJOR}.${${_PROJECT_NAME_X}_MINOR}.${${_PROJECT_NAME_X}_PATCH}")
message(STATUS "Detected ${_PROJECT_NAME_X} version: ${${_PROJECT_NAME_X}_VERSION}")

project(${_PROJECT_NAME}
    VERSION ${${_PROJECT_NAME_X}_VERSION}
    LANGUAGES C
)

# libaco's acosw.S is preprocessed assembly (x86_64/i386 context-switch stubs).
enable_language(ASM)

# ── Shared ufsrv build options ───────────────────────────────────────────────
# Clang enforcement, base compile flags, and the UFSRV_BUILD_WITH_{ASAN,MSAN,TSAN}
# toggles live in the shared module. Warning flags are THIS repo's own policy —
# set them before including the module. Invoke via a preset: `cmake --preset dev`.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    if(BUILD_SHARED_LIBS)
        set(_UFSRV_BUILD_FLAVOUR "SHARED+STATIC")
    else()
        set(_UFSRV_BUILD_FLAVOUR "STATIC")
    endif()
else()
    set(_UFSRV_BUILD_FLAVOUR "OBJECT")
endif()

list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake-modules)
set(UFSRV_C_WARNING_FLAGS
    -Wall -Wimplicit-fallthrough -Wno-unused-label -Wno-unused-variable
    -Wno-comment -Wno-nullability-completeness -Wno-unknown-attributes)
set(UFSRV_C_DEBUG_FLAGS_DEFAULT -ggdb3)
include(ufsrv_build_options)

add_definitions(-DHAVE_CONFIG_H)

# ── Config generation ────────────────────────────────────────────────────────

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/config.h
    @ONLY
)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(IS_ROOT_PROJECT ON)
else()
    set(IS_ROOT_PROJECT OFF)
endif()

option(_PACKAGE_TESTS "Enable test targets" OFF)
option(_PACKAGE_INSTALL "Enable install target" ${IS_ROOT_PROJECT})

# ── Dependencies ─────────────────────────────────────────────────────────────

if(NOT TARGET uflib::uflib)
    find_package(uflib REQUIRED)
endif()
message(STATUS "uflib dependency resolved via uflib::uflib")

# ── Build target — OBJECT library (dual-mode) ────────────────────────────────

add_library(${_PROJECT_NAME} OBJECT)

if(IS_ROOT_PROJECT)
    add_library(${_PROJECT_NAME}_static STATIC $<TARGET_OBJECTS:${_PROJECT_NAME}>)
    set_target_properties(${_PROJECT_NAME}_static PROPERTIES OUTPUT_NAME ${_PROJECT_NAME})

    if(BUILD_SHARED_LIBS)
        add_library(${_PROJECT_NAME}_shared SHARED $<TARGET_OBJECTS:${_PROJECT_NAME}>)
        set_target_properties(${_PROJECT_NAME}_shared PROPERTIES
            OUTPUT_NAME ${_PROJECT_NAME}
            VERSION   ${${_PROJECT_NAME_X}_VERSION}
            SOVERSION ${${_PROJECT_NAME_X}_MAJOR}
            C_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON)
        add_library(${_PROJECT_NAME}::${_PROJECT_NAME} ALIAS ${_PROJECT_NAME}_shared)
    else()
        add_library(${_PROJECT_NAME}::${_PROJECT_NAME} ALIAS ${_PROJECT_NAME}_static)
    endif()
else()
    add_library(${_PROJECT_NAME}::${_PROJECT_NAME} ALIAS ${_PROJECT_NAME})
endif()

ufsrv_apply_build_options(${_PROJECT_NAME})
ufsrv_build_summary(${_PROJECT_NAME})
if(IS_ROOT_PROJECT)
    ufsrv_apply_build_options(${_PROJECT_NAME}_static)
    ufsrv_build_summary(${_PROJECT_NAME}_static)
    if(BUILD_SHARED_LIBS)
        ufsrv_apply_build_options(${_PROJECT_NAME}_shared)
        ufsrv_build_summary(${_PROJECT_NAME}_shared)
    endif()
endif()

# ── Include directories ──────────────────────────────────────────────────────

target_include_directories(${_PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/ufsrvrxlib>
        $<INSTALL_INTERFACE:include>
        $<INSTALL_INTERFACE:include/ufsrvrxlib>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/src/ufsrv_scheduler
        ${CMAKE_CURRENT_SOURCE_DIR}/packages
)

# ── Propagate PUBLIC paths to installed targets ──────────────────────────────
# $<TARGET_OBJECTS> does NOT propagate INTERFACE_INCLUDE_DIRECTORIES or
# INTERFACE_LINK_LIBRARIES from the OBJECT library.  Set them explicitly on
# _static / _shared so find_package(ufsrvrxlib) consumers see the right paths.

set(_ufsrvrxlib_interface_includes
    $<INSTALL_INTERFACE:include>
    $<INSTALL_INTERFACE:include/ufsrvrxlib>
)
set(_ufsrvrxlib_interface_libs
    uflib::uflib
)

if(IS_ROOT_PROJECT)
    target_include_directories(${_PROJECT_NAME}_static INTERFACE ${_ufsrvrxlib_interface_includes})
    target_link_libraries(${_PROJECT_NAME}_static INTERFACE ${_ufsrvrxlib_interface_libs})
    if(BUILD_SHARED_LIBS)
        target_include_directories(${_PROJECT_NAME}_shared INTERFACE ${_ufsrvrxlib_interface_includes})
        target_link_libraries(${_PROJECT_NAME}_shared INTERFACE ${_ufsrvrxlib_interface_libs})
    endif()
endif()

# ── Link dependencies ────────────────────────────────────────────────────────

target_link_libraries(${_PROJECT_NAME} PUBLIC uflib::uflib)

set_target_properties(${_PROJECT_NAME} PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    OUTPUT_NAME ${_PROJECT_NAME}
)

# ── Vendored libaco (packages/libaco) — separate STATIC lib ────────────────
# Built in isolation from the main OBJECT target so the third-party context-
# switch assembly stays out of the sanitizer-instrumented ufsrvrxlib objects.
# The build-interface include dir exposes <libaco/aco.h> to the coroutine
# wrapper within the build tree only; it is not part of the installed interface.

add_library(ufsrvrxlib_aco STATIC
    packages/libaco/aco.c
    packages/libaco/acosw.S
)
target_include_directories(ufsrvrxlib_aco PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/packages>)
set_target_properties(ufsrvrxlib_aco PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(IS_ROOT_PROJECT)
    target_link_libraries(${_PROJECT_NAME}_static PRIVATE ufsrvrxlib_aco)
    if(BUILD_SHARED_LIBS)
        target_link_libraries(${_PROJECT_NAME}_shared PRIVATE ufsrvrxlib_aco)
    endif()
endif()

# ── Sources — explicit listing (centralized per ufsrv convention) ────────────
# Module sources are added as they land. The ufsrv_scheduler module is first;
# future/promise/coroutine modules follow as siblings.

target_sources(${_PROJECT_NAME} PRIVATE
    # ── Scheduler (src/ufsrv_scheduler/) ────────────────────────────────
    src/ufsrv_scheduler/ufsrv_scheduler.c

    # ── Coroutine (src/ufsrv_coroutine/) ───────────────────────────────
    src/ufsrv_coroutine/ufsrv_coroutine.c

    # ── Cancellation (src/ufsrv_cancellation/) ─────────────────────────
    src/ufsrv_cancellation/ufsrv_cancellation.c

    # ── Future (src/ufsrv_future/) ──────────────────────────────────────
    src/ufsrv_future/ufsrv_future.c
)

# ── Dynamic version — generated into build directory ─────────────────────────

add_custom_target(${_PROJECT_NAME}_version
    COMMAND chmod 755 ${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/src/version.c.sh
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/version.c
    COMMENT "Generating version.c with a fresh compilation timestamp"
    VERBATIM
)
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/version.c PROPERTIES GENERATED TRUE)
target_sources(${_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.c)
add_dependencies(${_PROJECT_NAME} ${_PROJECT_NAME}_version)

# ── Install ──────────────────────────────────────────────────────────────────

if(_PACKAGE_INSTALL)
    include(GNUInstallDirs)

    install(TARGETS ${_PROJECT_NAME}_static
        EXPORT ${_PROJECT_NAME}-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    install(TARGETS ufsrvrxlib_aco
        EXPORT ${_PROJECT_NAME}-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    if(BUILD_SHARED_LIBS)
        install(TARGETS ${_PROJECT_NAME}_shared
            EXPORT ${_PROJECT_NAME}-targets
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()

    install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

    # man page
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/docs/man/${_PROJECT_NAME}.7
        DESTINATION ${CMAKE_INSTALL_MANDIR}/man7
    )

    # pkg-config
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_PROJECT_NAME}.pc.in
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}.pc
        @ONLY
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )

    # cmake package config
    include(CMakePackageConfigHelpers)
    set(INSTALL_CONFIGDIR ${CMAKE_INSTALL_LIBDIR}/cmake/${_PROJECT_NAME})

    install(EXPORT ${_PROJECT_NAME}-targets
        FILE ${_PROJECT_NAME}Targets.cmake
        NAMESPACE ${_PROJECT_NAME}::
        DESTINATION ${INSTALL_CONFIGDIR}
    )

    set(${_PROJECT_NAME_X}_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include")

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/${_PROJECT_NAME}Config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Config.cmake
        INSTALL_DESTINATION ${INSTALL_CONFIGDIR}
        PATH_VARS ${_PROJECT_NAME_X}_INCLUDE_DIRS
    )

    set(CMAKE_EXPORT_PACKAGE_REGISTRY ON)
    export(PACKAGE ${_PROJECT_NAME})

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}ConfigVersion.cmake
        VERSION ${${_PROJECT_NAME_X}_VERSION}
        COMPATIBILITY AnyNewerVersion
    )

    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${_PROJECT_NAME}ConfigVersion.cmake
        DESTINATION ${INSTALL_CONFIGDIR}
    )

    # Uninstall target
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
        @ONLY
    )
    add_custom_target(uninstall
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake
    )

    # Install verification — informational only (never fails the build).
    # Compares the ufsrvrxlib version across system install, git HEAD, and local copy.
    add_custom_target(install_verification
        COMMAND ${CMAKE_COMMAND}
            -DINSTALL_PREFIX=${CMAKE_INSTALL_PREFIX}
            -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
            -P ${CMAKE_SOURCE_DIR}/cmake/install_verification.cmake
        COMMENT "Verifying ufsrvrxlib version across system install, git, and local copy"
        VERBATIM
    )

    # CPack (Debian packaging)
    set(CPACK_PACKAGE_NAME "${_PROJECT_NAME}")
    set(CPACK_PACKAGE_VERSION "${${_PROJECT_NAME_X}_VERSION}")
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ufsrv reactive-extensions library (coroutines, futures, scheduler)")
    set(CPACK_GENERATOR "DEB")
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "unfacd works")
    set(CPACK_DEBIAN_PACKAGE_SECTION "libs")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libuflib")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    include(CPack)
endif()

# ── Doxygen ──────────────────────────────────────────────────────────────────

if(IS_ROOT_PROJECT)
    find_package(Doxygen QUIET)
    if(Doxygen_FOUND)
        set(DOXYGEN_OUTPUT_DIR "${CMAKE_SOURCE_DIR}" CACHE PATH "Doxygen HTML output dir")
        configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Doxyfile.in
                       ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile @ONLY)
        add_custom_target(docs
            ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_BINARY_DIR}/Doxyfile
            VERBATIM)
    else()
        message(STATUS "Doxygen not found — 'docs' target disabled.")
    endif()
endif()

# ── Tests ────────────────────────────────────────────────────────────────────

if(_PACKAGE_TESTS)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)

    include(FetchContent)
    # Share one FetchContent clone across all build directories.
    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/build/_fetchcontent"
        CACHE PATH "Shared FetchContent download directory")
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG release-1.12.0
    )
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_subdirectory(tests)
endif()

# ── Examples ────────────────────────────────────────────────────────────────

add_subdirectory(examples)
