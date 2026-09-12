add_c_test(
    NAME ufsrv_scheduler_tests
    TEST_SOURCE ufsrv_scheduler_tests.cpp
    C_SOURCES
        ${CMAKE_SOURCE_DIR}/src/ufsrv_scheduler/ufsrv_scheduler.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}
    LIBRARIES
        uflib::uflib
)

# Standalone stress-test executable (not CTest-registered — run manually).
add_executable(ufsrv_scheduler_stress
    ufsrv_scheduler_stress.c
    ${CMAKE_SOURCE_DIR}/src/ufsrv_scheduler/ufsrv_scheduler.c
)
target_include_directories(ufsrv_scheduler_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(ufsrv_scheduler_stress PRIVATE uflib::uflib m)
ufsrv_apply_build_options(ufsrv_scheduler_stress)
