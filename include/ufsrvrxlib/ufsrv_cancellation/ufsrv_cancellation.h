/**
 * @file ufsrv_cancellation.h
 * @brief Cooperative cancellation token.
 *
 * A UfsrvCancellationToken is a shared flag that long-running work (e.g. a
 * coroutine awaiting a future) polls at yield points. Cancellation is
 * cooperative: the token is checked, never pre-empted. UfsrvFutureGetWithCancellation
 * honours it before waiting.
 *
 * The token also keeps a lock-free registry of one-shot callbacks that run on
 * UfsrvCancellationTokenCancel. A waiter suspended on the token registers a
 * callback that resumes it, so cancellation wakes the waiter (resume-on-cancel).
 */

#ifndef UFSRVRXLIB_UFSRV_CANCELLATION_H
#define UFSRVRXLIB_UFSRV_CANCELLATION_H

#include <uflib/standard_c_includes.h>

#include <ufsrvrxlib/ufsrvrxlib_defs.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Opaque handle to a cooperative cancellation token. */
typedef struct UfsrvCancellationToken UfsrvCancellationToken;

/*!
 * @brief One-shot callback run when a token is cancelled.
 *
 * @param[in] context_ptr  Opaque context captured at registration.
 */
typedef void (*UfsrvCancellationCallback)(void *context_ptr);

/*!
 * @brief Create a cancellation token (initially not cancelled).
 *
 * @return A new token, or NULL on allocation failure.
 */
PUBLIC_API UfsrvCancellationToken *UfsrvCancellationTokenCreate(void);

/*!
 * @brief Mark the token cancelled (idempotent).
 *
 * @param[in,out] token_ptr  Token to cancel (may be NULL, a no-op).
 */
PUBLIC_API void UfsrvCancellationTokenCancel(UfsrvCancellationToken *token_ptr);

/*!
 * @brief Query whether the token is cancelled.
 *
 * @param[in] token_ptr  Token to test (NULL → not cancelled).
 * @return true if the token is cancelled.
 */
PUBLIC_API bool UfsrvCancellationTokenIsCancelled(const UfsrvCancellationToken *token_ptr);

/*!
 * @brief Register a one-shot callback to run on cancellation.
 *
 * The callback runs exactly once — either on UfsrvCancellationTokenCancel, or
 * never if it is unregistered first. The returned handle is used to unregister.
 *
 * @param[in,out] token_ptr  Token to observe.
 * @param[in]     callback   Callback to run on cancel (non-NULL).
 * @param[in]     context_ptr  Opaque context passed to the callback.
 * @return An unregister handle, or NULL on invalid arguments / allocation failure.
 */
PUBLIC_API void *UfsrvCancellationTokenRegister(UfsrvCancellationToken *token_ptr, UfsrvCancellationCallback callback, void *context_ptr);

/*!
 * @brief Register a one-shot callback with a context-release hook.
 *
 * Like UfsrvCancellationTokenRegister, but `release_context` (if non-NULL) is called
 * with `context_ptr` when the callback node is finally reclaimed — on Cancel, on
 * Unregister, or on Destroy. Use it to drop a reference the callback's context owns
 * (e.g. a retain the registrant took to keep the context alive).
 *
 * @param[in,out] token_ptr  Token to observe.
 * @param[in]     callback   Callback to run on cancel (non-NULL).
 * @param[in]     context_ptr  Opaque context passed to the callback.
 * @param[in]     release_context  Optional context-release hook (may be NULL).
 * @return An unregister handle, or NULL on invalid arguments / allocation failure.
 */
PUBLIC_API void *UfsrvCancellationTokenRegisterEx(
    UfsrvCancellationToken *token_ptr,
    UfsrvCancellationCallback callback,
    void *context_ptr,
    UfsrvCancellationCallback release_context);

/*!
 * @brief Unregister (neutralise) a registered callback.
 *
 * Claims the callback so a later Cancel will not run it, and drops the
 * registrant's reference. Idempotent — safe to call more than once.
 *
 * @param[in] handle  Handle from UfsrvCancellationTokenRegister (NULL → false).
 * @return true if this call neutralised the callback (Cancel had not run it yet),
 *         false if the callback was already consumed (e.g. Cancel already ran it).
 */
PUBLIC_API bool UfsrvCancellationTokenUnregister(void *handle);

/*!
 * @brief Release the token.
 *
 * @param[in,out] token_ptr  Token to free (may be NULL).
 */
PUBLIC_API void UfsrvCancellationTokenDestroy(UfsrvCancellationToken *token_ptr);

#ifdef __cplusplus
}
#endif

#endif /* UFSRVRXLIB_UFSRV_CANCELLATION_H */
