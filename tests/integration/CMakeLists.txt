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
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(ufsrv_layers_e2e PRIVATE ${uflib_TARGET} ufsrvrxlib_aco m)
ufsrv_apply_build_options(ufsrv_layers_e2e)

add_executable(ufsrv_contract_stress
    ufsrv_contract_stress.c
    ${CMAKE_SOURCE_DIR}/src/ufsrv_scheduler/ufsrv_scheduler.c
    ${CMAKE_SOURCE_DIR}/src/ufsrv_future/ufsrv_future.c
    ${CMAKE_SOURCE_DIR}/src/ufsrv_coroutine/ufsrv_coroutine.c
    ${CMAKE_SOURCE_DIR}/src/ufsrv_cancellation/ufsrv_cancellation.c
)
target_include_directories(ufsrv_contract_stress PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/src
)
target_link_libraries(ufsrv_contract_stress PRIVATE ${uflib_TARGET} ufsrvrxlib_aco m)
ufsrv_apply_build_options(ufsrv_contract_stress)
