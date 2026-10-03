/**
 * @file ufsrv_cancellation_priv.h
 * @brief Private definitions for the ufsrv_cancellation implementation.
 *
 * Colocated with the implementation under src/ — never installed, never
 * included by consumers.
 */

#ifndef UFSRVRXLIB_UFSRV_CANCELLATION_PRIV_H
#define UFSRVRXLIB_UFSRV_CANCELLATION_PRIV_H

#include <pthread.h>
#include <stdatomic.h>

#include <ufsrvrxlib/ufsrv_cancellation/ufsrv_cancellation.h>

struct UfsrvCancellationCallback {
    struct UfsrvCancellationCallback *prev;
    struct UfsrvCancellationCallback *next;
    UfsrvCancellationToken           *token;
    int                               refcount;
    bool                              active;
    bool                              claimed;
    UfsrvCancellationCallback         fn;
    void                             *context;
    UfsrvCancellationCallback         release_context;
};

struct UfsrvCancellationToken {
    _Atomic(int)                      refcount;
    pthread_mutex_t                   lock;
    _Atomic(bool)                     is_cancelled;
    struct UfsrvCancellationCallback *head;
};

void UfsrvCancellationTokenRetainInternal(UfsrvCancellationToken *token_ptr);

#endif /* UFSRVRXLIB_UFSRV_CANCELLATION_PRIV_H */
