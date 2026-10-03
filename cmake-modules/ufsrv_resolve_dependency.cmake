include_guard(GLOBAL)

# Resolves <name> from an in-source packages/<name>, the system install (pkg-config, then
# CMake CONFIG), or a fetch, unless a target already exists; sets <name>_TARGET/_ORIGIN.

macro(ufsrv_resolve_dependency name pkgname)
    if(ARGC GREATER 2)
        set(_ufsrv_dep_prefix "${ARGV2}")
    else()
        set(_ufsrv_dep_prefix "${PROJECT_NAME}")
    endif()
    string(TOUPPER "${_ufsrv_dep_prefix}" _ufsrv_dep_prefix_upper)
    string(TOUPPER "${name}" _ufsrv_dep_name_upper)

    set(${_ufsrv_dep_prefix_upper}_${_ufsrv_dep_name_upper}_SOURCE
        "https://github.com/unfacd/${name}" CACHE STRING
        "Where to fetch ${name} from when it is neither in packages/${name} nor installed. A git URL, or a path to a local checkout.")
    set(${_ufsrv_dep_prefix_upper}_DEP_GIT_TAG "master" CACHE STRING
        "Branch or tag checked out when a dependency is fetched. Pin it for a reproducible build.")

    set(_ufsrv_dep_src "${${_ufsrv_dep_prefix_upper}_${_ufsrv_dep_name_upper}_SOURCE}")
    set(_ufsrv_dep_tag "${${_ufsrv_dep_prefix_upper}_DEP_GIT_TAG}")
    set(${name}_ORIGIN "")
    set(${name}_TARGET "")
    set(${name}_SYSTEM_VIA "")

    if(TARGET ${name}::${name} OR TARGET PkgConfig::${pkgname})
        set(${name}_ORIGIN PARENT)
        if(TARGET PkgConfig::${pkgname})
            set(${name}_TARGET "PkgConfig::${pkgname}")
        else()
            set(${name}_TARGET "${name}::${name}")
        endif()
    else()
        find_package(PkgConfig REQUIRED)
        include(FetchContent)

        set(_ufsrv_dep_saved_tests ${_PACKAGE_TESTS})
        set(_ufsrv_dep_saved_install ${_PACKAGE_INSTALL})
        set(_PACKAGE_TESTS OFF CACHE BOOL "" FORCE)
        set(_PACKAGE_INSTALL OFF CACHE BOOL "" FORCE)

        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/packages/${name}/CMakeLists.txt")
            message(STATUS "${name}: in-source checkout at packages/${name}")
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/packages/${name}"
                             "${CMAKE_CURRENT_BINARY_DIR}/packages/${name}")
            set(${name}_ORIGIN SOURCE)
        else()
            pkg_check_modules(${pkgname} IMPORTED_TARGET GLOBAL ${name})
            if(${pkgname}_FOUND)
                message(STATUS "${name}: system install (pkg-config, ${${pkgname}_VERSION})")
                set(${name}_ORIGIN SYSTEM)
                set(${name}_SYSTEM_VIA PKGCONFIG)
            else()
                find_package(${name} CONFIG QUIET
                    NO_CMAKE_PACKAGE_REGISTRY
                    NO_CMAKE_SYSTEM_PACKAGE_REGISTRY)
                if(${name}_FOUND)
                    message(STATUS "${name}: system install (CMake package, ${${name}_VERSION})")
                    set(${name}_ORIGIN SYSTEM)
                    set(${name}_SYSTEM_VIA CONFIG)
                elseif(IS_DIRECTORY "${_ufsrv_dep_src}")
                    message(STATUS "${name}: fetching from the local path ${_ufsrv_dep_src}")
                    FetchContent_Declare(${name} SOURCE_DIR "${_ufsrv_dep_src}")
                    FetchContent_MakeAvailable(${name})
                    set(${name}_ORIGIN FETCH)
                else()
                    message(STATUS "${name}: not installed; fetching ${_ufsrv_dep_src} (${_ufsrv_dep_tag})")
                    FetchContent_Declare(${name}
                        GIT_REPOSITORY "${_ufsrv_dep_src}"
                        GIT_TAG "${_ufsrv_dep_tag}"
                        GIT_SHALLOW TRUE)
                    FetchContent_MakeAvailable(${name})
                    set(${name}_ORIGIN FETCH)
                endif()
            endif()
        endif()

        set(_PACKAGE_TESTS ${_ufsrv_dep_saved_tests} CACHE BOOL "" FORCE)
        set(_PACKAGE_INSTALL ${_ufsrv_dep_saved_install} CACHE BOOL "" FORCE)

        if(${name}_SYSTEM_VIA STREQUAL "PKGCONFIG")
            set(${name}_TARGET "PkgConfig::${pkgname}")
        elseif(TARGET ${name}::${name})
            set(${name}_TARGET "${name}::${name}")
        else()
            message(FATAL_ERROR
                "${name} was resolved from ${${name}_ORIGIN} but defines no ${name}::${name} target.")
        endif()
    endif()

    set(UFSRV_RESOLVED_${_ufsrv_dep_name_upper}_ORIGIN "${${name}_ORIGIN}"
        CACHE INTERNAL "${name} resolution origin" FORCE)
    set(UFSRV_RESOLVED_${_ufsrv_dep_name_upper}_TARGET "${${name}_TARGET}"
        CACHE INTERNAL "${name} resolved target" FORCE)

    unset(_ufsrv_dep_prefix)
    unset(_ufsrv_dep_prefix_upper)
    unset(_ufsrv_dep_name_upper)
    unset(_ufsrv_dep_src)
    unset(_ufsrv_dep_tag)
    unset(_ufsrv_dep_saved_tests)
    unset(_ufsrv_dep_saved_install)
endmacro()
