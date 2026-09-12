/**
 * @file ufsrv_future.c
 * @brief Future/Promise with a lock-free (Treiber) continuation list.
 *
 * Copyright (C) 2015-2026 unfacd works
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <sys/eventfd.h>

#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>

#include "ufsrv_future_priv.h"

/*!
 * @brief Release one reference to a continuation node.
 *
 * @param[in,out] node_ptr  Node to release; freed when the last reference drops.
 */
static void
sNodeRelease(struct UfsrvContinuation *node_ptr)
{
    if (lockless_treiber_stack_release(&node_ptr->base)) {
        free(node_ptr);
    }
}

/*!
 * @brief Run a continuation exactly once (guarded by the node's claim flag).
 *
 * @param[in] future_ptr  Future whose result is delivered.
 * @param[in] node_ptr    Continuation to run.
 */
static void
sRunContinuation(UfsrvFuture *future_ptr, struct UfsrvContinuation *node_ptr)
{
    if (lockless_treiber_stack_claim(&node_ptr->base)) {
        node_ptr->fn(&future_ptr->result, node_ptr->user);
    }
}

/*!
 * @brief Push a continuation onto the lock-free (Treiber) stack.
 *
 * @param[in,out] future_ptr  Future owning the stack.
 * @param[in]     node_ptr    Continuation to push.
 */
static void
sPushContinuation(UfsrvFuture *future_ptr, struct UfsrvContinuation *node_ptr)
{
    lockless_treiber_stack_push(future_ptr->continuations, &node_ptr->base);
}

/*!
 * @brief Drain and run all pending continuations.
 *
 * @param[in,out] future_ptr  Future whose continuations are drained.
 */
static void
sDrainContinuations(UfsrvFuture *future_ptr)
{
    struct LocklessTreiberStackNode *node_ptr =
        lockless_treiber_stack_steal_all(future_ptr->continuations);

    while (node_ptr != NULL) {
        struct LocklessTreiberStackNode *next_ptr =
            atomic_load_explicit(&node_ptr->next, memory_order_relaxed);
        struct UfsrvContinuation *cont = (struct UfsrvContinuation *)node_ptr;
        sRunContinuation(future_ptr, cont);
        sNodeRelease(cont);
        node_ptr = next_ptr;
    }
}

/*!
 * @brief Drain and resume all waiters (suspended coroutines).
 *
 * @param[in,out] future_ptr  Future whose waiters are drained.
 */
static void
sDrainWaiters(UfsrvFuture *future_ptr)
{
    struct LocklessTreiberStackNode *node_ptr =
        lockless_treiber_stack_steal_all(future_ptr->waiters);

    while (node_ptr != NULL) {
        struct LocklessTreiberStackNode *next_ptr =
            atomic_load_explicit(&node_ptr->next, memory_order_relaxed);
        struct UfsrvWaiter *waiter = (struct UfsrvWaiter *)node_ptr;
        if (lockless_treiber_stack_claim(node_ptr)) {
            if (waiter->block_fd >= 0) {
                uint64_t one = 1;
                (void)write(waiter->block_fd, &one, sizeof(one));
            } else {
                UfsrvCoroutineSubmitResume(waiter->scheduler, waiter->coroutine);
            }
        }
        if (lockless_treiber_stack_release(node_ptr)) {
            free(waiter);
        }
        node_ptr = next_ptr;
    }
}

/*!
 * @brief Complete a future with a result, exactly once.
 *
 * The `is_completed` flag is the one-shot guard: the promise's completion and a
 * cancel-triggered completion race through it, and only one wins. `state` is the
 * READY publication (release store) so readers acquire the result.
 *
 * @param[in,out] future_ptr  Future to complete.
 * @param[in]     result      Result to publish.
 * @return true if this call completed the future; false if it was already completed.
 */
static bool
sFutureComplete(UfsrvFuture *future_ptr, UfsrvFutureResult result)
{
    if (atomic_exchange_explicit(&future_ptr->is_completed, true, memory_order_acq_rel)) {
        return false;   /* already completed (promise or cancel won) */
    }

    future_ptr->result = result;
    atomic_store_explicit(&future_ptr->state, UFSRV_FUTURE_READY, memory_order_release);

    /* The future is now complete: release the cancel callback's registrant reference
     * (R) so the callback node can be reclaimed by a later cancel / token destroy.
     * This breaks the future↔callback reference cycle. */
    if (future_ptr->cancel_handle != NULL) {
        UfsrvCancellationTokenUnregister(future_ptr->cancel_handle);
    }

    sDrainWaiters(future_ptr);
    sDrainContinuations(future_ptr);
    return true;
}

/*!
 * @brief Register a continuation (internal, non-const fn — may move).
 *
 * @param[in,out] future_ptr   Future to observe.
 * @param[in]     fn           Continuation.
 * @param[in]     context_ptr  Opaque context passed to the continuation.
 * @return true on success, false on NULL arguments or allocation failure.
 */
static bool
sFutureThen(UfsrvFuture *future_ptr, UfsrvFutureCallback fn, void *context_ptr)
{
    if (future_ptr == NULL || fn == NULL) {
        return false;
    }

    if (atomic_load_explicit(&future_ptr->state, memory_order_acquire) == UFSRV_FUTURE_READY) {
        fn(&future_ptr->result, context_ptr);
        return true;
    }

    struct UfsrvContinuation *node_ptr = calloc(1, sizeof(*node_ptr));
    if (node_ptr == NULL) {
        return false;
    }

    node_ptr->fn = fn;
    node_ptr->user = context_ptr;
    lockless_treiber_stack_node_init(&node_ptr->base);

    sPushContinuation(future_ptr, node_ptr);

    /* Completed while we pushed — claim-and-run to avoid a lost wakeup. */
    if (atomic_load_explicit(&future_ptr->state, memory_order_acquire) == UFSRV_FUTURE_READY) {
        sRunContinuation(future_ptr, node_ptr);
    }
    sNodeRelease(node_ptr);

    return true;
}

/*!
 * @brief Move-chain continuation: forward (move) the result to the out promise.
 *
 * @param[in,out] result_ptr   Source result (cleared after the move).
 * @param[in]     context_ptr  The output promise.
 */
static void
sMoveChainContinuation(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    UfsrvPromise *out_promise = context_ptr;
    UfsrvPromiseSetResult(out_promise, *result_ptr);
    *result_ptr = (UfsrvFutureResult){0};
}

UfsrvPromise *
UfsrvPromiseCreate(UfsrvFuture **future_out)
{
    if (future_out == NULL) {
        return NULL;
    }

    UfsrvFuture *future_ptr = calloc(1, sizeof(*future_ptr));
    if (future_ptr == NULL) {
        return NULL;
    }

    UfsrvPromise *promise_ptr = calloc(1, sizeof(*promise_ptr));
    if (promise_ptr == NULL) {
        free(future_ptr);
        return NULL;
    }

    atomic_init(&future_ptr->is_completed, false);
    atomic_init(&future_ptr->state, UFSRV_FUTURE_PENDING);
    future_ptr->continuations = lockless_treiber_stack_create();
    future_ptr->waiters = lockless_treiber_stack_create();
    if (future_ptr->continuations == NULL || future_ptr->waiters == NULL) {
        lockless_treiber_stack_destroy(future_ptr->continuations);   /* NULL-safe */
        lockless_treiber_stack_destroy(future_ptr->waiters);         /* NULL-safe */
        free(promise_ptr);
        free(future_ptr);
        return NULL;
    }
    atomic_init(&future_ptr->refcount, 2);   /* promise + caller */
    atomic_init(&promise_ptr->is_fulfilled, false);

    promise_ptr->future = future_ptr;
    *future_out = future_ptr;
    return promise_ptr;
}

void
UfsrvPromiseSetResult(UfsrvPromise *promise_ptr, UfsrvFutureResult result)
{
    if (promise_ptr == NULL) {
        return;
    }
    if (atomic_exchange_explicit(&promise_ptr->is_fulfilled, true, memory_order_acq_rel)) {
        /* Already fulfilled (double-set, set-after-abandon, or cancellation won):
         * consume the value so the caller never leaks it. */
        if (result.free_value != NULL && result.value != NULL) {
            result.free_value(result.value);
        }
        return;
    }

    UfsrvFuture *future_ptr = promise_ptr->future;
    if (!sFutureComplete(future_ptr, result)) {
        /* Cancellation completed the future first — the value is dropped. */
        if (result.free_value != NULL && result.value != NULL) {
            result.free_value(result.value);
        }
    }

    UfsrvFutureRelease(future_ptr);
    free(promise_ptr);
}

void
UfsrvPromiseSetValue(UfsrvPromise *promise_ptr, void *value_ptr, void (*free_value)(void *value_ptr))
{
    UfsrvFutureResult result = { .error = 0, .value = value_ptr, .free_value = free_value };
    UfsrvPromiseSetResult(promise_ptr, result);
}

void
UfsrvPromiseSetError(UfsrvPromise *promise_ptr, int error)
{
    UfsrvFutureResult result = { .error = error, .value = NULL, .free_value = NULL };
    UfsrvPromiseSetResult(promise_ptr, result);
}

void
UfsrvPromiseDestroy(UfsrvPromise *promise_ptr)
{
    if (promise_ptr == NULL) {
        return;
    }
    if (atomic_exchange_explicit(&promise_ptr->is_fulfilled, true, memory_order_acq_rel)) {
        return;   /* already completed */
    }
    UfsrvFutureRelease(promise_ptr->future);
    free(promise_ptr);
}

bool
UfsrvFutureThen(UfsrvFuture *future_ptr, UfsrvFutureCallback callback, void *context_ptr)
{
    if (future_ptr == NULL || callback == NULL) {
        return false;
    }
    return sFutureThen(future_ptr, callback, context_ptr);
}

bool
UfsrvFutureIsReady(const UfsrvFuture *future_ptr)
{
    if (future_ptr == NULL) {
        return false;
    }
    return atomic_load_explicit(&future_ptr->state, memory_order_acquire) == UFSRV_FUTURE_READY;
}

/*!
 * @brief Blocking get(): wait on an eventfd until the future completes.
 *
 * Used when UfsrvFutureGet is called on a plain OS thread (no scheduler-driven
 * coroutine). The completion drain writes the eventfd, which wakes the blocked read.
 *
 * @param[in,out] future_ptr  Future to wait on.
 * @return The result (borrowed).
 */
static UfsrvFutureResult
sFutureGetBlocking(UfsrvFuture *future_ptr)
{
    struct UfsrvWaiter *waiter = malloc(sizeof(*waiter));
    if (waiter == NULL) {
        return (UfsrvFutureResult){ .error = -1, .value = NULL, .free_value = NULL };
    }

    waiter->block_fd = eventfd(0, EFD_CLOEXEC);
    if (waiter->block_fd < 0) {
        free(waiter);
        return (UfsrvFutureResult){ .error = -1, .value = NULL, .free_value = NULL };
    }
    waiter->coroutine = NULL;
    waiter->scheduler = NULL;
    waiter->cancel_handle = NULL;
    lockless_treiber_stack_node_init(&waiter->base);

    lockless_treiber_stack_push(future_ptr->waiters, &waiter->base);

    if (atomic_load_explicit(&future_ptr->state, memory_order_acquire) == UFSRV_FUTURE_READY) {
        if (lockless_treiber_stack_claim(&waiter->base)) {
            /* Orphaned: the drainer never saw this waiter. Return without blocking.
             * The waiter stays on the stack; UfsrvFutureRelease closes its fd. */
            lockless_treiber_stack_release(&waiter->base);
            return future_ptr->result;
        }
        /* The drainer claimed (wrote) us — fall through to read (returns immediately). */
    }

    uint64_t value = 0;
    ssize_t n;
    do {
        n = read(waiter->block_fd, &value, sizeof(value));
    } while (n < 0 && errno == EINTR);
    (void)n;
    (void)value;

    close(waiter->block_fd);
    if (lockless_treiber_stack_release(&waiter->base)) {
        free(waiter);
    }
    return future_ptr->result;
}

UfsrvFutureResult
UfsrvFutureGet(UfsrvFuture *future_ptr)
{
    if (future_ptr == NULL) {
        return (UfsrvFutureResult){ .error = -1, .value = NULL, .free_value = NULL };
    }

    if (atomic_load_explicit(&future_ptr->state, memory_order_acquire) == UFSRV_FUTURE_READY) {
        return future_ptr->result;
    }

    UfsrvScheduler *scheduler = UfsrvCoroutineCurrentScheduler();
    if (scheduler == NULL) {
        /* Plain thread (or a non-scheduler coroutine): block on an eventfd. */
        return sFutureGetBlocking(future_ptr);
    }

    struct UfsrvWaiter *waiter = malloc(sizeof(*waiter));
    if (waiter == NULL) {
        return (UfsrvFutureResult){ .error = -1, .value = NULL, .free_value = NULL };
    }
    waiter->coroutine = UfsrvCoroutineCurrent();
    waiter->scheduler = scheduler;
    waiter->block_fd = -1;
    waiter->cancel_handle = NULL;
    lockless_treiber_stack_node_init(&waiter->base);

    lockless_treiber_stack_push(future_ptr->waiters, &waiter->base);

    if (atomic_load_explicit(&future_ptr->state, memory_order_acquire) == UFSRV_FUTURE_READY) {
        if (lockless_treiber_stack_claim(&waiter->base)) {
            /* Orphaned: the drainer never saw this waiter. Return without yielding. */
            lockless_treiber_stack_release(&waiter->base);
            return future_ptr->result;
        }
        /* The drainer claimed (resumed) us — fall through to yield. */
    }

    UfsrvCoroutineYield();

    if (lockless_treiber_stack_release(&waiter->base)) {
        free(waiter);
    }
    return future_ptr->result;
}

/*!
 * @brief Cancellation callback: resume a suspended waiter (exactly once).
 *
 * Runs on the cancelling thread. Resuming is claim-guarded on the waiter so that
 * a concurrent future completion (which also claims the waiter) cannot resume the
 * same coroutine twice.
 *
 * @param[in] context_ptr  The waiter (struct UfsrvWaiter) to resume.
 */
static void
sResumeWaiterOnCancel(void *context_ptr)
{
    struct UfsrvWaiter *waiter = context_ptr;

    if (lockless_treiber_stack_claim(&waiter->base)) {
        UfsrvCoroutineSubmitResume(waiter->scheduler, waiter->coroutine);
    }
}

/*!
 * @brief Cancellation callback for a blocking waiter: write its eventfd to wake it.
 *
 * The blocking analogue of sResumeWaiterOnCancel; claim-guarded so exactly one trigger
 * (the completion drain or the cancel drain) writes the fd.
 *
 * @param[in] context_ptr  The blocking waiter (struct UfsrvWaiter) to wake.
 */
static void
sWakeBlockingWaiterOnCancel(void *context_ptr)
{
    struct UfsrvWaiter *waiter = context_ptr;

    if (lockless_treiber_stack_claim(&waiter->base)) {
        uint64_t one = 1;
        (void)write(waiter->block_fd, &one, sizeof(one));
    }
}

/*!
 * @brief Release the blocking waiter's cancel reference (the callback node's
 *        release-context hook).
 *
 * @param[in] context_ptr  The blocking waiter (struct UfsrvWaiter) to release.
 */
static void
sWaiterCancelRelease(void *context_ptr)
{
    struct UfsrvWaiter *waiter = context_ptr;

    if (lockless_treiber_stack_release(&waiter->base)) {
        free(waiter);
    }
}

/*!
 * @brief Blocking + cancellable get(): wait on an eventfd until the future completes or
 *        the token is cancelled.
 *
 * @param[in,out] future_ptr  Future to wait on.
 * @param[in,out] token_ptr   Token to observe (non-NULL).
 * @return ECANCELED if cancelled, otherwise the future's result (borrowed).
 */
static UfsrvFutureResult
sFutureGetBlockingCancellable(UfsrvFuture *future_ptr, UfsrvCancellationToken *token_ptr)
{
    struct UfsrvWaiter *waiter = malloc(sizeof(*waiter));
    if (waiter == NULL) {
        return (UfsrvFutureResult){ .error = -1, .value = NULL, .free_value = NULL };
    }

    waiter->block_fd = eventfd(0, EFD_CLOEXEC);
    if (waiter->block_fd < 0) {
        free(waiter);
        return (UfsrvFutureResult){ .error = -1, .value = NULL, .free_value = NULL };
    }
    waiter->coroutine = NULL;
    waiter->scheduler = NULL;
    waiter->cancel_handle = NULL;
    lockless_treiber_stack_node_init(&waiter->base);

    lockless_treiber_stack_push(future_ptr->waiters, &waiter->base);

    /* A third reference (pusher + stack + cancel) keeps the waiter alive while the
     * cancel callback may still write its fd. */
    atomic_fetch_add_explicit(&waiter->base.refcount, 1, memory_order_relaxed);

    waiter->cancel_handle = UfsrvCancellationTokenRegisterEx(token_ptr, sWakeBlockingWaiterOnCancel, waiter, sWaiterCancelRelease);
    if (waiter->cancel_handle == NULL) {
        /* Roll back: cancel ref (3→2), then pusher ref (2→1); the stack ref (1) is
         * reclaimed by UfsrvFutureRelease. */
        lockless_treiber_stack_release(&waiter->base);
        close(waiter->block_fd);
        waiter->block_fd = -1;
        lockless_treiber_stack_release(&waiter->base);
        return (UfsrvFutureResult){ .error = -1, .value = NULL, .free_value = NULL };
    }

    bool r_released = false;

    /* Lost-wakeup guard (token): mirror UfsrvFutureGetWithCancellation. */
    if (UfsrvCancellationTokenIsCancelled(token_ptr)) {
        r_released = true;   /* Unregister below drops the registrant reference */
        if (UfsrvCancellationTokenUnregister(waiter->cancel_handle)) {
            /* We won: cancel won't run our callback. The callback node's registrant
             * reference is released; its stack reference is reclaimed at token destroy. */
            close(waiter->block_fd);
            waiter->block_fd = -1;
            lockless_treiber_stack_release(&waiter->base);   /* pusher ref */
            return (UfsrvFutureResult){ .error = ECANCELED, .value = NULL, .free_value = NULL };
        }
        /* Cancel already claimed the callback and will write the fd — fall through. */
    }

    /* Lost-wakeup guard (future): mirror sFutureGetBlocking's orphan handling. */
    if (atomic_load_explicit(&future_ptr->state, memory_order_acquire) == UFSRV_FUTURE_READY) {
        if (lockless_treiber_stack_claim(&waiter->base)) {
            /* Orphaned: the drainer never saw this waiter. Neutralise + return. */
            if (!r_released) {
                UfsrvCancellationTokenUnregister(waiter->cancel_handle);
            }
            lockless_treiber_stack_release(&waiter->base);   /* pusher ref */
            return future_ptr->result;
        }
        /* The drainer claimed (wrote) us — fall through to read (returns immediately). */
    }

    uint64_t value = 0;
    ssize_t n;
    do {
        n = read(waiter->block_fd, &value, sizeof(value));
    } while (n < 0 && errno == EINTR);
    (void)n;
    (void)value;

    close(waiter->block_fd);
    waiter->block_fd = -1;
    if (!r_released) {
        UfsrvCancellationTokenUnregister(waiter->cancel_handle);
    }
    if (lockless_treiber_stack_release(&waiter->base)) {
        free(waiter);
    }

    if (UfsrvCancellationTokenIsCancelled(token_ptr)) {
        return (UfsrvFutureResult){ .error = ECANCELED, .value = NULL, .free_value = NULL };
    }
    return future_ptr->result;
}

UfsrvFutureResult
UfsrvFutureGetWithCancellation(UfsrvFuture *future_ptr, UfsrvCancellationToken *token_ptr)
{
    if (future_ptr == NULL) {
        return (UfsrvFutureResult){ .error = -1, .value = NULL, .free_value = NULL };
    }
    if (token_ptr == NULL) {
        return UfsrvFutureGet(future_ptr);
    }
    if (UfsrvCancellationTokenIsCancelled(token_ptr)) {
        return (UfsrvFutureResult){ .error = ECANCELED, .value = NULL, .free_value = NULL };
    }
    if (atomic_load_explicit(&future_ptr->state, memory_order_acquire) == UFSRV_FUTURE_READY) {
        return future_ptr->result;
    }

    if (UfsrvCoroutineCurrentScheduler() == NULL) {
        /* Plain thread: block on an eventfd and observe the token. */
        return sFutureGetBlockingCancellable(future_ptr, token_ptr);
    }

    struct UfsrvWaiter *waiter = malloc(sizeof(*waiter));
    if (waiter == NULL) {
        return (UfsrvFutureResult){ .error = -1, .value = NULL, .free_value = NULL };
    }
    waiter->coroutine = UfsrvCoroutineCurrent();
    waiter->scheduler = UfsrvCoroutineCurrentScheduler();
    waiter->block_fd = -1;
    waiter->cancel_handle = NULL;
    lockless_treiber_stack_node_init(&waiter->base);

    lockless_treiber_stack_push(future_ptr->waiters, &waiter->base);

    /* A third reference (pusher + stack + cancel) keeps the waiter alive while the
     * cancel callback may still resume it. */
    atomic_fetch_add_explicit(&waiter->base.refcount, 1, memory_order_relaxed);

    waiter->cancel_handle = UfsrvCancellationTokenRegisterEx(token_ptr, sResumeWaiterOnCancel, waiter, sWaiterCancelRelease);
    if (waiter->cancel_handle == NULL) {
        /* Roll back: cancel ref (3→2), then pusher ref (2→1); the stack ref (1) is
         * reclaimed by UfsrvFutureRelease. */
        lockless_treiber_stack_release(&waiter->base);
        lockless_treiber_stack_release(&waiter->base);
        return (UfsrvFutureResult){ .error = -1, .value = NULL, .free_value = NULL };
    }

    bool r_released = false;

    /* Lost-wakeup guard (token): if cancelled concurrently with our registration,
     * Cancel may have drained before our callback was pushed (orphaning it). Claim
     * the callback to tell the two cases apart: winning means nobody will resume us. */
    if (UfsrvCancellationTokenIsCancelled(token_ptr)) {
        r_released = true;   /* Unregister below drops the registrant reference */
        if (UfsrvCancellationTokenUnregister(waiter->cancel_handle)) {
            /* We won: cancel won't run our callback. Drop the pusher reference; the
             * stack reference (waiter is pushed) and cancel reference (reclaimed at
             * token destroy) are released elsewhere. */
            lockless_treiber_stack_release(&waiter->base);
            return (UfsrvFutureResult){ .error = ECANCELED, .value = NULL, .free_value = NULL };
        }
        /* Cancel already claimed the callback and will resume us — fall through. */
    }

    /* Lost-wakeup guard (future): mirrors UfsrvFutureGet's orphan handling. */
    if (atomic_load_explicit(&future_ptr->state, memory_order_acquire) == UFSRV_FUTURE_READY) {
        if (lockless_treiber_stack_claim(&waiter->base)) {
            /* Orphaned: the drainer never saw this waiter. Neutralise + return. */
            if (!r_released) {
                UfsrvCancellationTokenUnregister(waiter->cancel_handle);
            }
            lockless_treiber_stack_release(&waiter->base);
            return future_ptr->result;
        }
        /* The drainer claimed (resumed) us — fall through to yield. */
    }

    UfsrvCoroutineYield();

    /* Resumed (by completion or by cancel). Neutralise the cancel callback (drops
     * the registrant reference) and the waiter's pusher reference. */
    if (!r_released) {
        UfsrvCancellationTokenUnregister(waiter->cancel_handle);
    }
    if (lockless_treiber_stack_release(&waiter->base)) {
        free(waiter);
    }

    if (UfsrvCancellationTokenIsCancelled(token_ptr)) {
        return (UfsrvFutureResult){ .error = ECANCELED, .value = NULL, .free_value = NULL };
    }
    return future_ptr->result;
}

UfsrvFuture *
UfsrvFutureFromValue(void *value_ptr, void (*free_value)(void *value_ptr))
{
    UfsrvFuture *future_ptr = NULL;
    UfsrvPromise *promise_ptr = UfsrvPromiseCreate(&future_ptr);
    if (promise_ptr == NULL) {
        return NULL;
    }
    UfsrvPromiseSetValue(promise_ptr, value_ptr, free_value);
    return future_ptr;
}

UfsrvFuture *
UfsrvFutureFromError(int error)
{
    UfsrvFuture *future_ptr = NULL;
    UfsrvPromise *promise_ptr = UfsrvPromiseCreate(&future_ptr);
    if (promise_ptr == NULL) {
        return NULL;
    }
    UfsrvPromiseSetError(promise_ptr, error);
    return future_ptr;
}

void
UfsrvFutureRetain(UfsrvFuture *future_ptr)
{
    if (future_ptr == NULL) {
        return;
    }
    atomic_fetch_add_explicit(&future_ptr->refcount, 1, memory_order_relaxed);
}

void
UfsrvFutureRelease(UfsrvFuture *future_ptr)
{
    if (future_ptr == NULL) {
        return;
    }
    if (atomic_fetch_sub_explicit(&future_ptr->refcount, 1, memory_order_acq_rel) != 1) {
        return;
    }

    if (future_ptr->result.free_value != NULL && future_ptr->result.value != NULL) {
        future_ptr->result.free_value(future_ptr->result.value);
    }

    struct LocklessTreiberStackNode *node_ptr =
        lockless_treiber_stack_steal_all(future_ptr->continuations);
    while (node_ptr != NULL) {
        struct LocklessTreiberStackNode *next_ptr =
            atomic_load_explicit(&node_ptr->next, memory_order_relaxed);
        free(node_ptr);   /* base is the first member → frees the continuation */
        node_ptr = next_ptr;
    }

    node_ptr = lockless_treiber_stack_steal_all(future_ptr->waiters);
    while (node_ptr != NULL) {
        struct LocklessTreiberStackNode *next_ptr =
            atomic_load_explicit(&node_ptr->next, memory_order_relaxed);
        struct UfsrvWaiter *waiter = (struct UfsrvWaiter *)node_ptr;
        if (waiter->block_fd >= 0) {
            close(waiter->block_fd);
        }
        free(node_ptr);   /* base is the first member → frees the waiter */
        node_ptr = next_ptr;
    }

    lockless_treiber_stack_destroy(future_ptr->continuations);
    lockless_treiber_stack_destroy(future_ptr->waiters);
    free(future_ptr);
}

/*!
 * @brief Cancellation callback: complete the attached future with ECANCELED.
 *
 * @param[in] context_ptr  The future (struct UfsrvFuture) to complete.
 */
static void
sFutureCancelCallback(void *context_ptr)
{
    UfsrvFuture *future_ptr = context_ptr;

    sFutureComplete(future_ptr, (UfsrvFutureResult){ .error = ECANCELED, .value = NULL, .free_value = NULL });
}

/*!
 * @brief Cancellation context-release: drop the future's cancel reference.
 *
 * @param[in] context_ptr  The future (struct UfsrvFuture) whose reference is released.
 */
static void
sFutureCancelRelease(void *context_ptr)
{
    UfsrvFutureRelease(context_ptr);
}

bool
UfsrvFutureAttachCancellation(UfsrvFuture *future_ptr, UfsrvCancellationToken *token_ptr)
{
    if (future_ptr == NULL || token_ptr == NULL) {
        return false;
    }
    if (future_ptr->cancel_token != NULL) {
        return false;   /* one token per future */
    }

    /* If the future is already completed, there is nothing left to cancel. */
    if (atomic_load_explicit(&future_ptr->is_completed, memory_order_acquire)) {
        return false;
    }

    /* The cancel reference keeps the future alive until the callback is reclaimed
     * (on cancel or token destroy), so a cancel after release is still safe. */
    UfsrvFutureRetain(future_ptr);

    void *handle = UfsrvCancellationTokenRegisterEx(token_ptr, sFutureCancelCallback, future_ptr, sFutureCancelRelease);
    if (handle == NULL) {
        UfsrvFutureRelease(future_ptr);   /* roll back the retain */
        return false;
    }

    future_ptr->cancel_handle = handle;
    future_ptr->cancel_token = token_ptr;

    /* Lost-wakeup guard: if the token is already cancelled, Cancel may have drained
     * before our callback was registered (orphaning it). Completing here is exactly-once
     * (guarded by is_completed) and sFutureComplete reclaims the callback node. */
    if (UfsrvCancellationTokenIsCancelled(token_ptr)) {
        sFutureComplete(future_ptr, (UfsrvFutureResult){ .error = ECANCELED, .value = NULL, .free_value = NULL });
    }

    return true;
}

/*! Context for the map continuation. */
struct UfsrvMapContext {
    UfsrvPromise           *out_promise;
    UfsrvFutureMapCallback  mapper;
    void                   *user;
};

static void
sMapContinuation(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    struct UfsrvMapContext *ctx = context_ptr;

    if (result_ptr->error != 0) {
        UfsrvPromiseSetError(ctx->out_promise, result_ptr->error);
    } else {
        void *mapped = ctx->mapper(result_ptr->value, ctx->user);
        if (mapped == NULL) {
            UfsrvPromiseSetError(ctx->out_promise, -1);
        } else {
            UfsrvPromiseSetValue(ctx->out_promise, mapped, free);
        }
    }
    free(ctx);
}

UfsrvFuture *
UfsrvFutureMap(UfsrvFuture *future_ptr, UfsrvFutureMapCallback mapper, void *context_ptr)
{
    if (future_ptr == NULL || mapper == NULL) {
        return NULL;
    }

    UfsrvFuture *out_future = NULL;
    UfsrvPromise *out_promise = UfsrvPromiseCreate(&out_future);
    if (out_promise == NULL) {
        return NULL;
    }

    struct UfsrvMapContext *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }

    ctx->out_promise = out_promise;
    ctx->mapper = mapper;
    ctx->user = context_ptr;

    if (!sFutureThen(future_ptr, sMapContinuation, ctx)) {
        free(ctx);
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }

    if (future_ptr->cancel_token != NULL) {
        UfsrvFutureAttachCancellation(out_future, future_ptr->cancel_token);
    }

    return out_future;
}

/*! Context for the flatMap continuation. */
struct UfsrvFlatMapContext {
    UfsrvPromise                *out_promise;
    UfsrvFutureFlatMapCallback   mapper;
    void                        *user;
    UfsrvCancellationToken      *token;   /*!< Outer future's token (borrowed; may be NULL). */
};

static void
sFlatMapContinuation(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    struct UfsrvFlatMapContext *ctx = context_ptr;

    if (result_ptr->error != 0) {
        UfsrvPromiseSetError(ctx->out_promise, result_ptr->error);
        free(ctx);
        return;
    }

    UfsrvFuture *next = ctx->mapper(result_ptr->value, ctx->user);
    if (next == NULL) {
        UfsrvPromiseSetError(ctx->out_promise, -1);
        free(ctx);
        return;
    }

    /* Propagate cancellation across the async boundary: cancel the inner future too. */
    if (ctx->token != NULL) {
        UfsrvFutureAttachCancellation(next, ctx->token);
    }

    sFutureThen(next, sMoveChainContinuation, ctx->out_promise);
    UfsrvFutureRelease(next);
    free(ctx);
}

UfsrvFuture *
UfsrvFutureFlatMap(UfsrvFuture *future_ptr, UfsrvFutureFlatMapCallback mapper, void *context_ptr)
{
    if (future_ptr == NULL || mapper == NULL) {
        return NULL;
    }

    UfsrvFuture *out_future = NULL;
    UfsrvPromise *out_promise = UfsrvPromiseCreate(&out_future);
    if (out_promise == NULL) {
        return NULL;
    }

    struct UfsrvFlatMapContext *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }

    ctx->out_promise = out_promise;
    ctx->mapper = mapper;
    ctx->user = context_ptr;
    ctx->token = future_ptr->cancel_token;

    if (!sFutureThen(future_ptr, sFlatMapContinuation, ctx)) {
        free(ctx);
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }

    if (future_ptr->cancel_token != NULL) {
        UfsrvFutureAttachCancellation(out_future, future_ptr->cancel_token);
    }

    return out_future;
}

/*! Context for the onError continuation. */
struct UfsrvOnErrorContext {
    UfsrvPromise               *out_promise;
    UfsrvFutureRecoverCallback  recovery;
    void                       *user;
};

static void
sOnErrorContinuation(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    struct UfsrvOnErrorContext *ctx = context_ptr;

    if (result_ptr->error != 0) {
        UfsrvFuture *recovered = ctx->recovery(result_ptr->error, ctx->user);
        if (recovered == NULL) {
            UfsrvPromiseSetError(ctx->out_promise, result_ptr->error);
            free(ctx);
            return;
        }
        sFutureThen(recovered, sMoveChainContinuation, ctx->out_promise);
        UfsrvFutureRelease(recovered);
        free(ctx);
        return;
    }

    /* Success: forward (move) the value unchanged. */
    UfsrvPromiseSetResult(ctx->out_promise, *result_ptr);
    *result_ptr = (UfsrvFutureResult){0};
    free(ctx);
}

UfsrvFuture *
UfsrvFutureOnError(UfsrvFuture *future_ptr, UfsrvFutureRecoverCallback recovery, void *context_ptr)
{
    if (future_ptr == NULL || recovery == NULL) {
        return NULL;
    }

    UfsrvFuture *out_future = NULL;
    UfsrvPromise *out_promise = UfsrvPromiseCreate(&out_future);
    if (out_promise == NULL) {
        return NULL;
    }

    struct UfsrvOnErrorContext *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }

    ctx->out_promise = out_promise;
    ctx->recovery = recovery;
    ctx->user = context_ptr;

    if (!sFutureThen(future_ptr, sOnErrorContinuation, ctx)) {
        free(ctx);
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }

    if (future_ptr->cancel_token != NULL) {
        UfsrvFutureAttachCancellation(out_future, future_ptr->cancel_token);
    }

    return out_future;
}

/*! Context for the doOnSuccess continuation. */
struct UfsrvDoOnSuccessContext {
    UfsrvPromise              *out_promise;
    UfsrvFutureActionCallback  action;
    void                      *user;
};

static void
sDoOnSuccessContinuation(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    struct UfsrvDoOnSuccessContext *ctx = context_ptr;

    if (result_ptr->error == 0) {
        ctx->action(result_ptr->value, ctx->user);
    }

    /* Forward (move) the result — value on success, error otherwise. */
    UfsrvPromiseSetResult(ctx->out_promise, *result_ptr);
    *result_ptr = (UfsrvFutureResult){0};
    free(ctx);
}

UfsrvFuture *
UfsrvFutureDoOnSuccess(UfsrvFuture *future_ptr, UfsrvFutureActionCallback action, void *context_ptr)
{
    if (future_ptr == NULL || action == NULL) {
        return NULL;
    }

    UfsrvFuture *out_future = NULL;
    UfsrvPromise *out_promise = UfsrvPromiseCreate(&out_future);
    if (out_promise == NULL) {
        return NULL;
    }

    struct UfsrvDoOnSuccessContext *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }

    ctx->out_promise = out_promise;
    ctx->action = action;
    ctx->user = context_ptr;

    if (!sFutureThen(future_ptr, sDoOnSuccessContinuation, ctx)) {
        free(ctx);
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }

    if (future_ptr->cancel_token != NULL) {
        UfsrvFutureAttachCancellation(out_future, future_ptr->cancel_token);
    }

    return out_future;
}

/*! Context for UfsrvFutureAll. */
struct UfsrvAllContext {
    UfsrvPromise *out_promise;
    _Atomic(size_t) remaining;
    _Atomic(bool)   failed;
};

static void
sAllContinuation(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    struct UfsrvAllContext *ctx = context_ptr;

    if (result_ptr->error != 0 && !atomic_exchange_explicit(&ctx->failed, true, memory_order_acq_rel)) {
        UfsrvPromiseSetError(ctx->out_promise, result_ptr->error);   /* fail-fast */
    }

    if (atomic_fetch_sub_explicit(&ctx->remaining, 1, memory_order_acq_rel) == 1) {
        if (!atomic_load_explicit(&ctx->failed, memory_order_acquire)) {
            UfsrvPromiseSetValue(ctx->out_promise, NULL, NULL);
        }
        free(ctx);
    }
}

UfsrvFuture *
UfsrvFutureAll(UfsrvFuture **futures, size_t count)
{
    if (count == 0) {
        return UfsrvFutureFromValue(NULL, NULL);
    }
    if (futures == NULL) {
        return NULL;
    }

    UfsrvFuture *out_future = NULL;
    UfsrvPromise *out_promise = UfsrvPromiseCreate(&out_future);
    if (out_promise == NULL) {
        return NULL;
    }

    struct UfsrvAllContext *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }
    ctx->out_promise = out_promise;
    atomic_init(&ctx->remaining, count);
    atomic_init(&ctx->failed, false);

    for (size_t i = 0; i < count; i++) {
        UfsrvFutureThen(futures[i], sAllContinuation, ctx);
    }

    return out_future;
}

/*! Context for UfsrvFutureAny. */
struct UfsrvAnyContext {
    UfsrvPromise *out_promise;
    _Atomic(bool)   claimed;
    _Atomic(size_t) remaining;
};

static void
sAnyContinuation(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    struct UfsrvAnyContext *ctx = context_ptr;

    if (!atomic_exchange_explicit(&ctx->claimed, true, memory_order_acq_rel)) {
        UfsrvPromiseSetResult(ctx->out_promise, *result_ptr);
        *result_ptr = (UfsrvFutureResult){0};   /* move: winner takes ownership */
    }

    if (atomic_fetch_sub_explicit(&ctx->remaining, 1, memory_order_acq_rel) == 1) {
        free(ctx);
    }
}

UfsrvFuture *
UfsrvFutureAny(UfsrvFuture **futures, size_t count)
{
    if (count == 0) {
        return UfsrvFutureFromValue(NULL, NULL);
    }
    if (futures == NULL) {
        return NULL;
    }

    UfsrvFuture *out_future = NULL;
    UfsrvPromise *out_promise = UfsrvPromiseCreate(&out_future);
    if (out_promise == NULL) {
        return NULL;
    }

    struct UfsrvAnyContext *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }
    ctx->out_promise = out_promise;
    atomic_init(&ctx->claimed, false);
    atomic_init(&ctx->remaining, count);

    for (size_t i = 0; i < count; i++) {
        UfsrvFutureThen(futures[i], sAnyContinuation, ctx);
    }

    return out_future;
}

/*! Context for UfsrvFutureZip. */
struct UfsrvZipContext {
    UfsrvPromise           *out_promise;
    UfsrvFutureZipCallback  zipper;
    void                   *user;
    UfsrvFutureResult       first;
    UfsrvFutureResult       second;
    _Atomic(size_t)           remaining;
};

static void
sZipComplete(struct UfsrvZipContext *ctx)
{
    if (ctx->first.error != 0) {
        UfsrvPromiseSetError(ctx->out_promise, ctx->first.error);
    } else if (ctx->second.error != 0) {
        UfsrvPromiseSetError(ctx->out_promise, ctx->second.error);
    } else {
        void *zipped = ctx->zipper(ctx->first.value, ctx->second.value, ctx->user);
        if (zipped == NULL) {
            UfsrvPromiseSetError(ctx->out_promise, -1);
        } else {
            UfsrvPromiseSetValue(ctx->out_promise, zipped, free);
        }
    }
    free(ctx);
}

static void
sZipFirstContinuation(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    struct UfsrvZipContext *ctx = context_ptr;
    ctx->first = *result_ptr;   /* borrow */
    if (atomic_fetch_sub_explicit(&ctx->remaining, 1, memory_order_acq_rel) == 1) {
        sZipComplete(ctx);
    }
}

static void
sZipSecondContinuation(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    struct UfsrvZipContext *ctx = context_ptr;
    ctx->second = *result_ptr;
    if (atomic_fetch_sub_explicit(&ctx->remaining, 1, memory_order_acq_rel) == 1) {
        sZipComplete(ctx);
    }
}

UfsrvFuture *
UfsrvFutureZip(UfsrvFuture *future_a, UfsrvFuture *future_b, UfsrvFutureZipCallback zipper, void *context_ptr)
{
    if (future_a == NULL || future_b == NULL || zipper == NULL) {
        return NULL;
    }

    UfsrvFuture *out_future = NULL;
    UfsrvPromise *out_promise = UfsrvPromiseCreate(&out_future);
    if (out_promise == NULL) {
        return NULL;
    }

    struct UfsrvZipContext *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        UfsrvPromiseDestroy(out_promise);
        UfsrvFutureRelease(out_future);
        return NULL;
    }
    ctx->out_promise = out_promise;
    ctx->zipper = zipper;
    ctx->user = context_ptr;
    atomic_init(&ctx->remaining, 2);

    if (!UfsrvFutureThen(future_a, sZipFirstContinuation, ctx) ||
        !UfsrvFutureThen(future_b, sZipSecondContinuation, ctx)) {
        /* Partial registration (OOM): leak ctx/promise/future — the registered
         * continuation owns ctx and frees it via the remaining counter. */
        return NULL;
    }

    return out_future;
}
