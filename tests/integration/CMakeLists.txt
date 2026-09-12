# Standalone end-to-end test (not CTest-registered — run manually).
add_executable(ufsrv_layers_e2e
    ufsrv_layers_e2e.c
    ${CMAKE_SOURCE_DIR}/src/ufsrv_scheduler/ufsrv_scheduler.c
    ${CMAKE_SOURCE_DIR}/src/ufsrv_future/ufsrv_future.c
    ${CMAKE_SOURCE_DIR}/src/ufsrv_coroutine/ufsrv_coroutine.c
    ${CMAKE_SOURCE_DIR}/src/ufsrv_cancellation/ufsrv_cancellation.c
)
target_include_directories(ufsrv_layers_e2e PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(ufsrv_layers_e2e PRIVATE uflib::uflib ufsrvrxlib_aco m)
ufsrv_apply_build_options(ufsrv_layers_e2e)
