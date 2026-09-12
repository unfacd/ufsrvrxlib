/**
 * @file ufsrv_future_type.h
 * @brief Public types for the ufsrv_future module.
 */

#ifndef UFSRVRXLIB_UFSRV_FUTURE_TYPE_H
#define UFSRVRXLIB_UFSRV_FUTURE_TYPE_H

#include <uflib/standard_c_includes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Opaque handle to a future (consumer side). */
typedef struct UfsrvFuture UfsrvFuture;

/*! Opaque handle to a promise (producer side). */
typedef struct UfsrvPromise UfsrvPromise;

/*!
 * Result delivered by a completed future.
 *
 * Ownership: `value` is owned by the future. When the last reference to the
 * future is released it is freed via `free_value` (if both are non-NULL).
 * A zero `error` denotes success. Continuations receive this by pointer and
 * may either *borrow* it (const, read-only) or *move* it (copy then zero the
 * source) — see UfsrvFutureFlatMap/UfsrvFutureOnError.
 */
typedef struct UfsrvFutureResult {
    int   error;                          /*!< 0 = success, non-zero = error. */
    void *value;                          /*!< Success payload (owned). */
    void (*free_value)(void *value_ptr);  /*!< Releases `value`; may be NULL. */
} UfsrvFutureResult;

/*!
 * Terminal continuation invoked when a future completes. Borrows the result — it
 * must not modify `*result_ptr` or take ownership of the value (a documented
 * contract; the future still owns the value).
 *
 * @param[in] result_ptr   Result (borrowed; the future still owns it).
 * @param[in] context_ptr  User context passed to the registration call.
 */
typedef void (*UfsrvFutureCallback)(UfsrvFutureResult *result_ptr, void *context_ptr);

/*!
 * Synchronous value transform for UfsrvFutureMap. Borrows the input value.
 *
 * @param[in] value_ptr    Success value (borrowed).
 * @param[in] context_ptr  User context.
 * @return A new value owned by the mapped future (freed with `free`), or NULL.
 */
typedef void *(*UfsrvFutureMapCallback)(const void *value_ptr, void *context_ptr);

/*!
 * Asynchronous transform for UfsrvFutureFlatMap. Borrows the input value.
 *
 * @param[in] value_ptr    Success value (borrowed).
 * @param[in] context_ptr  User context.
 * @return The inner future whose result becomes the flatMap result, or NULL.
 */
typedef UfsrvFuture *(*UfsrvFutureFlatMapCallback)(const void *value_ptr, void *context_ptr);

/*!
 * Error recovery for UfsrvFutureOnError.
 *
 * @param[in] error        The propagated error code.
 * @param[in] context_ptr  User context.
 * @return A recovery future whose result replaces the error, or NULL.
 */
typedef UfsrvFuture *(*UfsrvFutureRecoverCallback)(int error, void *context_ptr);

/*!
 * Success-only side effect for UfsrvFutureDoOnSuccess. Borrows the value.
 *
 * @param[in] value_ptr    Success value (borrowed).
 * @param[in] context_ptr  User context.
 */
typedef void (*UfsrvFutureActionCallback)(const void *value_ptr, void *context_ptr);

/*!
 * Zipper for UfsrvFutureZip: combine two success values into a new value.
 *
 * @param[in] value_a_ptr  First value (borrowed).
 * @param[in] value_b_ptr  Second value (borrowed).
 * @param[in] context_ptr  User context.
 * @return A new value owned by the zip result (freed with `free`), or NULL.
 */
typedef void *(*UfsrvFutureZipCallback)(const void *value_a_ptr, const void *value_b_ptr, void *context_ptr);

#ifdef __cplusplus
}
#endif

#endif /* UFSRVRXLIB_UFSRV_FUTURE_TYPE_H */
