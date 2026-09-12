add_c_test(
    NAME ufsrv_future_tests
    TEST_SOURCE ufsrv_future_tests.cpp
    C_SOURCES
        ${CMAKE_SOURCE_DIR}/src/ufsrv_future/ufsrv_future.c
        ${CMAKE_SOURCE_DIR}/src/ufsrv_coroutine/ufsrv_coroutine.c
        ${CMAKE_SOURCE_DIR}/src/ufsrv_scheduler/ufsrv_scheduler.c
        ${CMAKE_SOURCE_DIR}/src/ufsrv_cancellation/ufsrv_cancellation.c
    INCLUDE_DIRS
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}
    LIBRARIES
        uflib::uflib
        ufsrvrxlib_aco
)
