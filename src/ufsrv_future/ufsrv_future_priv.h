/**
 * @file ufsrv_future_priv.h
 * @brief Private definitions for the ufsrv_future implementation.
 */

#ifndef UFSRVRXLIB_UFSRV_FUTURE_PRIV_H
#define UFSRVRXLIB_UFSRV_FUTURE_PRIV_H

#include <stdatomic.h>

#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>
#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine_type.h>
#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler_type.h>

/*! Future completion state. */
enum {
    UFSRV_FUTURE_PENDING = 0,  /*!< Not yet completed. */
    UFSRV_FUTURE_READY   = 1   /*!< Result published. */
};

/*!
 * A registered continuation: embeds the lock-free stack node plus its payload.
 *
 * The continuation is typed non-const so operator continuations may *move* the
 * result (copy then zero the source); `then` callbacks borrow by contract.
 */
struct UfsrvContinuation {
    struct LocklessTreiberStackNode base;
    UfsrvFutureCallback                fn;
    void                             *user;
};

/*!
 * A waiter: a suspended coroutine (or a blocked OS thread) waiting on the future
 * via UfsrvFutureGet. Coroutine waiters carry their coroutine/scheduler; blocking
 * waiters carry an eventfd that the completion drain writes to wake them.
 */
struct UfsrvWaiter {
    struct LocklessTreiberStackNode base;
    UfsrvCoroutine                    *coroutine;      /*!< Coroutine path. */
    UfsrvScheduler                    *scheduler;      /*!< Coroutine path. */
    int                                block_fd;       /*!< Blocking path (-1 = coroutine waiter). */
    void                              *cancel_handle;  /*!< Registered cancel callback (NULL if none). */
};

/*! A future: result + continuation and waiter stacks + a refcount. */
struct UfsrvFuture {
    _Atomic(bool)               is_completed;   /*!< One-shot completion guard (promise vs cancel). */
    _Atomic(int)                state;          /*!< PENDING/READY publication. */
    UfsrvFutureResult           result;
    LocklessTreiberStack       *continuations;   /*!< Opaque continuation stack. */
    LocklessTreiberStack       *waiters;         /*!< Opaque waiter stack. */
    _Atomic(int)                refcount;
    UfsrvCancellationToken     *cancel_token;   /*!< Attached token (borrowed; caller-owned). */
    void                       *cancel_handle;  /*!< Registered cancel callback (NULL if none). */
};

/*! A promise: one-shot producer holding a reference to its future. */
struct UfsrvPromise {
    UfsrvFuture                *future;
    _Atomic(bool)               is_fulfilled;
};

#endif /* UFSRVRXLIB_UFSRV_FUTURE_PRIV_H */
