/**
 * @file ufsrv_cancellation.c
 * @brief Cooperative cancellation token.
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

#include <ufsrvrxlib/ufsrv_cancellation/ufsrv_cancellation.h>
#include <uflib/cdt/lockless_treiber_stack/lockless_treiber_stack.h>

struct UfsrvCancellationToken {
    _Atomic(bool)         is_cancelled;
    LocklessTreiberStack *callbacks;   /*!< Opaque callback stack. */
};

/*! A registered one-shot callback node (embedded stack node + payload). */
struct UfsrvCancellationCallback {
    struct LocklessTreiberStackNode base;
    UfsrvCancellationCallback         fn;
    void                             *context;
    UfsrvCancellationCallback         release_context;   /*!< Optional; runs on final free. */
};

/*!
 * @brief Release one reference to a callback node; free it (and run its
 *        release-context hook) when the last reference drops.
 *
 * @param[in,out] cb  Callback node to release.
 */
static void
sReleaseCallback(struct UfsrvCancellationCallback *cb)
{
    if (lockless_treiber_stack_release(&cb->base)) {
        if (cb->release_context != NULL) {
            cb->release_context(cb->context);
        }
        free(cb);
    }
}

UfsrvCancellationToken *
UfsrvCancellationTokenCreate(void)
{
    UfsrvCancellationToken *token = calloc(1, sizeof(*token));
    if (token == NULL) {
        return NULL;
    }
    atomic_init(&token->is_cancelled, false);
    token->callbacks = lockless_treiber_stack_create();
    if (token->callbacks == NULL) {
        free(token);
        return NULL;
    }
    return token;
}

void *
UfsrvCancellationTokenRegister(UfsrvCancellationToken *token_ptr, UfsrvCancellationCallback callback, void *context_ptr)
{
    return UfsrvCancellationTokenRegisterEx(token_ptr, callback, context_ptr, NULL);
}

void *
UfsrvCancellationTokenRegisterEx(UfsrvCancellationToken *token_ptr, UfsrvCancellationCallback callback,
                                 void *context_ptr, UfsrvCancellationCallback release_context)
{
    if (token_ptr == NULL || callback == NULL) {
        return NULL;
    }

    struct UfsrvCancellationCallback *cb = malloc(sizeof(*cb));
    if (cb == NULL) {
        return NULL;
    }
    lockless_treiber_stack_node_init(&cb->base);
    cb->fn = callback;
    cb->context = context_ptr;
    cb->release_context = release_context;
    lockless_treiber_stack_push(token_ptr->callbacks, &cb->base);
    return cb;
}

bool
UfsrvCancellationTokenUnregister(void *handle)
{
    if (handle == NULL) {
        return false;
    }
    struct UfsrvCancellationCallback *cb = handle;
    bool won = lockless_treiber_stack_claim(&cb->base);
    sReleaseCallback(cb);
    return won;
}

void
UfsrvCancellationTokenCancel(UfsrvCancellationToken *token_ptr)
{
    if (token_ptr == NULL) {
        return;
    }
    /* Idempotent: the acq_rel exchange makes the drain happen exactly once. */
    if (atomic_exchange_explicit(&token_ptr->is_cancelled, true, memory_order_acq_rel)) {
        return;
    }

    struct LocklessTreiberStackNode *node =
        lockless_treiber_stack_steal_all(token_ptr->callbacks);
    while (node != NULL) {
        struct LocklessTreiberStackNode *next =
            atomic_load_explicit(&node->next, memory_order_relaxed);
        struct UfsrvCancellationCallback *cb = (struct UfsrvCancellationCallback *)node;
        if (lockless_treiber_stack_claim(node)) {
            cb->fn(cb->context);
        }
        sReleaseCallback(cb);   /* drop the token-stack reference */
        node = next;
    }
}

bool
UfsrvCancellationTokenIsCancelled(const UfsrvCancellationToken *token_ptr)
{
    if (token_ptr == NULL) {
        return false;
    }
    return atomic_load_explicit(&token_ptr->is_cancelled, memory_order_acquire);
}

void
UfsrvCancellationTokenDestroy(UfsrvCancellationToken *token_ptr)
{
    if (token_ptr == NULL) {
        return;
    }

    /* Reclaim dead callback nodes still linked in the stack (see design Known
     * Issues: a Treiber stack has no remove, so completed-but-un-cancelled waiters
     * leave their callback nodes here until cancel/destroy). */
    struct LocklessTreiberStackNode *node =
        lockless_treiber_stack_steal_all(token_ptr->callbacks);
    while (node != NULL) {
        struct LocklessTreiberStackNode *next =
            atomic_load_explicit(&node->next, memory_order_relaxed);
        struct UfsrvCancellationCallback *cb = (struct UfsrvCancellationCallback *)node;
        lockless_treiber_stack_claim(node);
        sReleaseCallback(cb);   /* drop the token-stack reference */
        node = next;
    }

    lockless_treiber_stack_destroy(token_ptr->callbacks);
    free(token_ptr);
}
