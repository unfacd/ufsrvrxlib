# tests/CMakeLists.txt — gtest harness

add_library(gtest_shared_main OBJECT main.cpp)
target_link_libraries(gtest_shared_main PUBLIC GTest::gtest GTest::gtest_main)
ufsrv_apply_build_options(gtest_shared_main)

function(add_c_test)
    set(options "")
    set(oneValueArgs NAME TEST_SOURCE)
    set(multiValueArgs C_SOURCES INCLUDE_DIRS LIBRARIES)
    cmake_parse_arguments(TEST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    add_executable(${TEST_NAME}
        ${TEST_TEST_SOURCE}
        $<TARGET_OBJECTS:gtest_shared_main>
        ${TEST_C_SOURCES}
    )
    set_target_properties(${TEST_NAME} PROPERTIES LINKER_LANGUAGE CXX)
    target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDE_DIRS})
    target_link_libraries(${TEST_NAME} PRIVATE GTest::gtest GTest::gtest_main stdc++ m ${TEST_LIBRARIES})
    ufsrv_apply_build_options(${TEST_NAME})
    # C++ test files include C headers that use Apple Blocks (e.g. uflib
    # main_types.h → utils_str.h) — need -fblocks for CXX too.
    target_compile_options(${TEST_NAME} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fblocks>)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endfunction()

add_subdirectory(ufsrv_scheduler)
add_subdirectory(ufsrv_future)
add_subdirectory(ufsrv_coroutine)
add_subdirectory(integration)
