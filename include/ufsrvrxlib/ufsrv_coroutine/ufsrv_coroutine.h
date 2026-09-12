/**
 * @file ufsrv_coroutine.h
 * @brief Cooperative coroutines (thin wrapper over libaco, via uflib).
 *
 * A coroutine is a stackful unit of execution that runs to a `yield`/`exit` and
 * is resumed later. UfsrvCoroutineCreate binds the coroutine to the calling
 * thread; resume it on that same thread (the scheduler integration re-resumes a
 * suspended coroutine by submitting it back to its worker).
 *
 * @code{.c}
 * static void worker(void) {
 *     int *arg = UfsrvCoroutineGetArg();
 *     *arg = 1;
 *     UfsrvCoroutineYield();
 *     *arg = 2;
 *     UfsrvCoroutineExit();
 * }
 *
 * int value = 0;
 * UfsrvCoroutine *co = UfsrvCoroutineCreate(worker, &value);
 * UfsrvCoroutineResume(co);   // runs to the first yield  (value == 1)
 * UfsrvCoroutineResume(co);   // runs to exit and destroys (value == 2)
 * @endcode
 */

#ifndef UFSRVRXLIB_UFSRV_COROUTINE_H
#define UFSRVRXLIB_UFSRV_COROUTINE_H

#include <ufsrvrxlib/ufsrvrxlib_defs.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine_type.h>
#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler_type.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @brief Create a coroutine bound to the calling thread.
 *
 * The coroutine is not started; UfsrvCoroutineResume runs it until it yields or
 * exits. It must be resumed on the thread that created it.
 *
 * @param[in] entry   Entry point (non-NULL).
 * @param[in] arg_ptr Argument passed to the entry (via UfsrvCoroutineGetArg).
 * @return A new UfsrvCoroutine, or NULL on allocation failure.
 */
PUBLIC_API UfsrvCoroutine *UfsrvCoroutineCreate(UfsrvCoroutineEntry entry, void *arg_ptr);

/*!
 * @brief Resume a suspended coroutine.
 *
 * Runs the coroutine until it yields or exits. If it exited, the coroutine is
 * destroyed and the handle must not be used again.
 *
 * @param[in,out] coroutine_ptr  Coroutine to resume (may be NULL, a no-op).
 */
PUBLIC_API void UfsrvCoroutineResume(UfsrvCoroutine *coroutine_ptr);

/*!
 * @brief Suspend the current coroutine, returning to its creator.
 */
PUBLIC_API void UfsrvCoroutineYield(void);

/*!
 * @brief The currently-running coroutine (or the thread's main coroutine).
 *
 * @return The current coroutine handle.
 */
PUBLIC_API UfsrvCoroutine *UfsrvCoroutineCurrent(void);

/*!
 * @brief The argument passed to the current coroutine's entry.
 *
 * @return The entry argument (void pointer).
 */
PUBLIC_API void *UfsrvCoroutineGetArg(void);

/*!
 * @brief The scheduler the current coroutine is pinned to.
 *
 * Set when a coroutine is spawned or resumed on a worker thread; valid inside a
 * coroutine running on that worker.
 *
 * @return The scheduler, or NULL if not running in a scheduler-driven coroutine.
 */
PUBLIC_API UfsrvScheduler *UfsrvCoroutineCurrentScheduler(void);

/*!
 * @brief Terminate the current coroutine (does not return).
 */
PUBLIC_API void UfsrvCoroutineExit(void);

/*!
 * @brief Release the calling thread's coroutine resources.
 *
 * Destroys the thread's main coroutine and shared stack created lazily by
 * UfsrvCoroutineCreate. Idempotent. Call once, before a worker thread exits,
 * after all its coroutines have been destroyed.
 */
PUBLIC_API void UfsrvCoroutineThreadCleanup(void);

/*!
 * @brief Re-submit a suspended coroutine for resumption on a worker thread.
 *
 * Submits a job to the scheduler that resumes coroutine_ptr. Used to continue a
 * coroutine that suspended itself (e.g. inside UfsrvFutureGet) once its wait
 * condition is met.
 *
 * @param[in,out] scheduler_ptr   Scheduler whose worker will resume the coroutine.
 * @param[in,out] coroutine_ptr   Suspended coroutine (may be NULL, a no-op).
 */
PUBLIC_API void UfsrvCoroutineSubmitResume(UfsrvScheduler *scheduler_ptr, UfsrvCoroutine *coroutine_ptr);

/*!
 * @brief Spawn a coroutine on a worker thread (create + first resume).
 *
 * Submits a job to the scheduler that creates coroutine_ptr bound to the worker's
 * main coroutine and resumes it. The coroutine's entry argument (arg_ptr) is owned
 * by the caller and must outlive the coroutine.
 *
 * @param[in,out] scheduler_ptr  Scheduler to run the coroutine on.
 * @param[in]     entry          Entry point (non-NULL).
 * @param[in]     arg_ptr        Entry argument (via UfsrvCoroutineGetArg).
 */
PUBLIC_API void UfsrvCoroutineSpawn(UfsrvScheduler *scheduler_ptr, UfsrvCoroutineEntry entry, void *arg_ptr);

#ifdef __cplusplus
}
#endif

#endif /* UFSRVRXLIB_UFSRV_COROUTINE_H */
