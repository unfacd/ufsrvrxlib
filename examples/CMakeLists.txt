# Example: asynchronously download a random image while showing a progress bar.
add_executable(async_image_download async_image_download.c)
target_include_directories(async_image_download PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(async_image_download PRIVATE ufsrvrxlib::ufsrvrxlib)
ufsrv_apply_build_options(async_image_download)

# Example: live number slots, driven by coroutines (cooperative or parallel).
add_executable(slots slots.c)
target_include_directories(slots PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(slots PRIVATE ufsrvrxlib::ufsrvrxlib)
ufsrv_apply_build_options(slots)

# Example: a plain thread blocks on UfsrvFutureGet for a background result.
add_executable(blocking_get blocking_get.c)
target_include_directories(blocking_get PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(blocking_get PRIVATE ufsrvrxlib::ufsrvrxlib)
ufsrv_apply_build_options(blocking_get)

# Example: cooperative cancellation via a token attached to a future.
add_executable(cancellation cancellation.c)
target_include_directories(cancellation PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(cancellation PRIVATE ufsrvrxlib::ufsrvrxlib)
ufsrv_apply_build_options(cancellation)

# Example: cancellation propagates through a flatMap chain (option 2).
add_executable(cancellation_chain cancellation_chain.c)
target_include_directories(cancellation_chain PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(cancellation_chain PRIVATE ufsrvrxlib::ufsrvrxlib)
ufsrv_apply_build_options(cancellation_chain)

# Example: a plain thread blocks with a cancellation timeout (blocking + cancellable).
add_executable(blocking_cancellable blocking_cancellable.c)
target_include_directories(blocking_cancellable PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}
)
target_link_libraries(blocking_cancellable PRIVATE ufsrvrxlib::ufsrvrxlib)
ufsrv_apply_build_options(blocking_cancellable)
