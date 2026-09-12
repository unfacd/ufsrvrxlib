/**
 * @file ufsrv_future.h
 * @brief Future/Promise: composable, non-blocking result delivery.
 *
 * A UfsrvPromise is the one-shot producer; a UfsrvFuture is the consumer.
 * Completion runs registered continuations and downstream operators on the
 * completing thread. The continuation list is a lock-free (Treiber) stack.
 *
 * Operators either *borrow* the result (const, read-only: UfsrvFutureThen,
 * UfsrvFutureMap, UfsrvFutureDoOnSuccess) or *move* it (copy then zero the
 * source: UfsrvFutureFlatMap, UfsrvFutureOnError) so a value is freed exactly
 * once. A move continuation is the sole consumer of its input future.
 *
 * @code{.c}
 * UfsrvFuture *future = NULL;
 * UfsrvPromise *promise = UfsrvPromiseCreate(&future);
 *
 * UfsrvFuture *mapped = UfsrvFutureMap(future, transform, NULL);
 * UfsrvFutureThen(mapped, on_done, NULL);       // terminal
 * UfsrvPromiseSetValue(promise, payload, free); // completes + frees promise
 *
 * UfsrvFutureRelease(mapped);
 * UfsrvFutureRelease(future);
 * @endcode
 */

#ifndef UFSRVRXLIB_UFSRV_FUTURE_H
#define UFSRVRXLIB_UFSRV_FUTURE_H

#include <ufsrvrxlib/ufsrvrxlib_defs.h>
#include <ufsrvrxlib/ufsrv_future/ufsrv_future_type.h>
#include <ufsrvrxlib/ufsrv_cancellation/ufsrv_cancellation.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Create a linked promise/future pair.
 *
 * The future is returned with reference count 2 (one for the promise, one for
 * the caller). The promise must be completed with UfsrvPromiseSet* or abandoned
 * with UfsrvPromiseDestroy.
 *
 * @param[out] future_out  Receives the future (must be non-NULL; owned by caller).
 * @return A new UfsrvPromise, or NULL on NULL future_out / allocation failure.
 */
PUBLIC_API UfsrvPromise *UfsrvPromiseCreate(UfsrvFuture **future_out);

/*!
 * @brief Complete the promise with a success value (one-shot; frees the promise).
 *
 * @param[in,out] promise_ptr  Promise to complete.
 * @param[in]     value_ptr    Success value (ownership transferred).
 * @param[in]     free_value   Releases value_ptr; may be NULL.
 */
PUBLIC_API void UfsrvPromiseSetValue(UfsrvPromise *promise_ptr, void *value_ptr, void (*free_value)(void *value_ptr));

/*!
 * @brief Complete the promise with an error (one-shot; frees the promise).
 *
 * @param[in,out] promise_ptr  Promise to complete.
 * @param[in]     error        Non-zero error code.
 */
PUBLIC_API void UfsrvPromiseSetError(UfsrvPromise *promise_ptr, int error);

/*!
 * @brief Complete the promise with a full result (one-shot; frees the promise).
 *
 * @param[in,out] promise_ptr  Promise to complete.
 * @param[in]     result       Result (ownership transferred to the future).
 */
PUBLIC_API void UfsrvPromiseSetResult(UfsrvPromise *promise_ptr, UfsrvFutureResult result);

/*!
 * @brief Abandon the promise without completing it (frees the promise).
 *
 * The future remains valid but never completes; the caller still owns its
 * reference and must release it. Must not be called after UfsrvPromiseSet*.
 *
 * @param[in,out] promise_ptr  Promise to abandon.
 */
PUBLIC_API void UfsrvPromiseDestroy(UfsrvPromise *promise_ptr);

/*!
 * @brief Register a terminal (borrow) continuation.
 *
 * Runs immediately on the calling thread if the future is already ready,
 * otherwise when the future completes.
 *
 * @param[in,out] future_ptr   Future to observe.
 * @param[in]     callback     Continuation (non-NULL).
 * @param[in]     context_ptr  Opaque context passed to the callback.
 * @return true on success, false on NULL arguments or allocation failure.
 */
PUBLIC_API bool UfsrvFutureThen(UfsrvFuture *future_ptr, UfsrvFutureCallback callback, void *context_ptr);

/*!
 * @brief Query whether the future has completed.
 *
 * @param[in] future_ptr  Future to test.
 * @return true if the future is ready.
 */
PUBLIC_API bool UfsrvFutureIsReady(const UfsrvFuture *future_ptr);

/*!
 * @brief Cooperatively wait for the future's result.
 *
 * If not ready, suspends the current coroutine (via UfsrvCoroutineYield) and
 * resumes it when the future completes. Must be called from within a coroutine
 * running on a scheduler (see UfsrvCoroutineSpawn). The returned result is
 * borrowed — the future still owns the value and frees it on release.
 *
 * @param[in,out] future_ptr  Future to wait on.
 * @return The result (borrowed), or an error result on NULL/OOM.
 */
PUBLIC_API UfsrvFutureResult UfsrvFutureGet(UfsrvFuture *future_ptr);

/*!
 * @brief Cooperatively wait for the future's result, honouring a cancellation token.
 *
 * If the token is already cancelled, returns ECANCELED immediately without waiting.
 * If the token is cancelled while the coroutine is suspended, the coroutine is
 * resumed and returns ECANCELED. Otherwise behaves like UfsrvFutureGet. Must be
 * called from within a coroutine running on a scheduler.
 *
 * @param[in,out] future_ptr  Future to wait on.
 * @param[in,out] token_ptr   Optional cancellation token (NULL → no cancellation).
 * @return The result (borrowed), an ECANCELED result, or an error result on NULL/OOM.
 */
PUBLIC_API UfsrvFutureResult UfsrvFutureGetWithCancellation(UfsrvFuture *future_ptr, UfsrvCancellationToken *token_ptr);

/*!
 * @brief Attach a cancellation token to a future.
 *
 * When the token is cancelled, the future completes with ECANCELED, racing the
 * promise's completion (exactly one wins). The future retains itself for the
 * lifetime of the registered callback and releases that reference when the
 * callback is reclaimed (on cancel or token destroy) — so a cancel that fires
 * after the future is otherwise released remains safe. The token is borrowed
 * (caller-owned). A future may carry at most one token.
 *
 * @param[in,out] future_ptr  Future to make cancellable.
 * @param[in,out] token_ptr   Token to observe (non-NULL).
 * @return true on success, false on NULL args, an already-attached future, or OOM.
 */
PUBLIC_API bool UfsrvFutureAttachCancellation(UfsrvFuture *future_ptr, UfsrvCancellationToken *token_ptr);

/*!
 * @brief Create an already-completed successful future.
 *
 * @param[in] value_ptr   Value to wrap (ownership transferred).
 * @param[in] free_value  Releases value_ptr; may be NULL.
 * @return A ready future, or NULL on allocation failure.
 */
PUBLIC_API UfsrvFuture *UfsrvFutureFromValue(void *value_ptr, void (*free_value)(void *value_ptr));

/*!
 * @brief Create an already-completed failed future.
 *
 * @param[in] error  Error code.
 * @return A ready future, or NULL on allocation failure.
 */
PUBLIC_API UfsrvFuture *UfsrvFutureFromError(int error);

/*!
 * @brief Synchronously transform the success value (borrow; new value).
 *
 * The mapper runs only on success; errors propagate unchanged. The mapped value
 * is owned by the returned future and freed with `free`. The input future is
 * not consumed — the caller retains ownership.
 *
 * @param[in,out] future_ptr   Input future.
 * @param[in]     mapper       Transform (non-NULL).
 * @param[in]     context_ptr  Opaque context passed to the mapper.
 * @return A new future, or NULL on allocation failure.
 */
PUBLIC_API UfsrvFuture *UfsrvFutureMap(UfsrvFuture *future_ptr, UfsrvFutureMapCallback mapper, void *context_ptr);

/*!
 * @brief Chain another asynchronous operation (move).
 *
 * On success, calls the mapper with the value; the mapper's returned inner
 * future's result becomes this future's result (ownership moved). On error,
 * the error propagates unchanged.
 *
 * @param[in,out] future_ptr   Input future.
 * @param[in]     mapper       Async transform (non-NULL).
 * @param[in]     context_ptr  Opaque context passed to the mapper.
 * @return A new future, or NULL on allocation failure.
 */
PUBLIC_API UfsrvFuture *UfsrvFutureFlatMap(UfsrvFuture *future_ptr, UfsrvFutureFlatMapCallback mapper, void *context_ptr);

/*!
 * @brief Recover from an error (move on success, recovery future on error).
 *
 * On success the value is forwarded (moved). On error, the recovery callback
 * produces a future whose result replaces the error.
 *
 * @param[in,out] future_ptr   Input future.
 * @param[in]     recovery     Recovery callback (non-NULL).
 * @param[in]     context_ptr  Opaque context passed to the recovery.
 * @return A new future, or NULL on allocation failure.
 */
PUBLIC_API UfsrvFuture *UfsrvFutureOnError(UfsrvFuture *future_ptr, UfsrvFutureRecoverCallback recovery, void *context_ptr);

/*!
 * @brief Run a success-only side effect, then forward the result (move).
 *
 * @param[in,out] future_ptr   Input future.
 * @param[in]     action       Side effect (non-NULL).
 * @param[in]     context_ptr  Opaque context passed to the action.
 * @return A new future carrying the original result, or NULL on allocation failure.
 */
PUBLIC_API UfsrvFuture *UfsrvFutureDoOnSuccess(UfsrvFuture *future_ptr, UfsrvFutureActionCallback action, void *context_ptr);

/*!
 * @brief Wait for all futures to complete (fail-fast).
 *
 * The result completes successfully (no value) once every input succeeds, or
 * with the first error as soon as one fails. The inputs' values remain owned by
 * their futures — the caller still releases them.
 *
 * @param[in,out] futures  Array of input futures.
 * @param[in]     count    Number of inputs (0 → immediate success).
 * @return A new future, or NULL on invalid arguments / allocation failure.
 */
PUBLIC_API UfsrvFuture *UfsrvFutureAll(UfsrvFuture **futures, size_t count);

/*!
 * @brief Combine two futures with a zipper.
 *
 * On both success, calls the zipper with the two values (borrowed) to produce a
 * new value (owned by the result). On either error, the error propagates.
 *
 * @param[in,out] future_a    First input future.
 * @param[in,out] future_b    Second input future.
 * @param[in]     zipper      Combine function (non-NULL).
 * @param[in]     context_ptr Opaque context passed to the zipper.
 * @return A new future, or NULL on invalid arguments / allocation failure.
 */
PUBLIC_API UfsrvFuture *UfsrvFutureZip(UfsrvFuture *future_a, UfsrvFuture *future_b, UfsrvFutureZipCallback zipper, void *context_ptr);

/*!
 * @brief Complete with the first future to finish.
 *
 * The winner's result (value or error) is moved to the returned future; the
 * other inputs' values remain owned by their futures.
 *
 * @param[in,out] futures  Array of input futures.
 * @param[in]     count    Number of inputs (> 0).
 * @return A new future, or NULL on invalid arguments / allocation failure.
 */
PUBLIC_API UfsrvFuture *UfsrvFutureAny(UfsrvFuture **futures, size_t count);

/*!
 * @brief Retain an additional reference to the future.
 *
 * @param[in,out] future_ptr  Future to retain.
 */
PUBLIC_API void UfsrvFutureRetain(UfsrvFuture *future_ptr);

/*!
 * @brief Release a reference to the future.
 *
 * Frees the future (and its result value) when the last reference is dropped.
 *
 * @param[in,out] future_ptr  Future to release (may be NULL).
 */
PUBLIC_API void UfsrvFutureRelease(UfsrvFuture *future_ptr);

#ifdef __cplusplus
}
#endif

#endif /* UFSRVRXLIB_UFSRV_FUTURE_H */
