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

#include "ufsrv_cancellation_priv.h"

/*!
 * @brief Free a node and the token reference its registration handle held.
 *
 * @param[in,out] cb  Node whose reference count has reached zero.
 */
static void
sTokenRelease(UfsrvCancellationToken *token_ptr);

static void
sNodeFree(struct UfsrvCancellationCallback *cb)
{
    UfsrvCancellationToken *token_ptr = cb->token;
    UfsrvCancellationCallback release_context = cb->release_context;
    void *context_ptr = cb->context;

    free(cb);
    if (release_context != NULL) {
        release_context(context_ptr);
    }
    sTokenRelease(token_ptr);
}

/*!
 * @brief Drop one node reference; free the node when that was the last.
 *
 * @param[in,out] cb  Node to release.
 * @return true if this call freed the node.
 */
static bool
sNodeDropRef(struct UfsrvCancellationCallback *cb)
{
    pthread_mutex_lock(&cb->token->lock);
    bool last = (--cb->refcount == 0);
    pthread_mutex_unlock(&cb->token->lock);

    if (last) {
        sNodeFree(cb);
    }
    return last;
}

static void
sTokenRelease(UfsrvCancellationToken *token_ptr)
{
    if (atomic_fetch_sub_explicit(&token_ptr->refcount, 1, memory_order_acq_rel) != 1) {
        return;
    }

    pthread_mutex_lock(&token_ptr->lock);
    struct UfsrvCancellationCallback *node = token_ptr->head;
    token_ptr->head = NULL;
    pthread_mutex_unlock(&token_ptr->lock);

    while (node != NULL) {
        struct UfsrvCancellationCallback *next = node->next;
        UfsrvCancellationCallback release_context = node->release_context;
        void *context_ptr = node->context;

        if (release_context != NULL) {
            release_context(context_ptr);
        }
        free(node);
        node = next;
    }

    pthread_mutex_destroy(&token_ptr->lock);
    free(token_ptr);
}

void
UfsrvCancellationTokenRetainInternal(UfsrvCancellationToken *token_ptr)
{
    if (unlikely(token_ptr == NULL)) {
        return;
    }
    atomic_fetch_add_explicit(&token_ptr->refcount, 1, memory_order_relaxed);
}

UfsrvCancellationToken *
UfsrvCancellationTokenCreate(void)
{
    UfsrvCancellationToken *token_ptr = calloc(1, sizeof(*token_ptr));
    if (token_ptr == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&token_ptr->lock, NULL) != 0) {
        free(token_ptr);
        return NULL;
    }
    atomic_init(&token_ptr->refcount, 1);
    atomic_init(&token_ptr->is_cancelled, false);
    token_ptr->head = NULL;
    return token_ptr;
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
    if (unlikely(token_ptr == NULL || callback == NULL)) {
        return NULL;
    }

    struct UfsrvCancellationCallback *cb = calloc(1, sizeof(*cb));
    if (cb == NULL) {
        return NULL;
    }

    cb->fn = callback;
    cb->context = context_ptr;
    cb->release_context = release_context;
    cb->token = token_ptr;
    cb->refcount = 2;   /* the registration handle + the token's list */

    UfsrvCancellationTokenRetainInternal(token_ptr);

    pthread_mutex_lock(&token_ptr->lock);
    bool already_cancelled = atomic_load_explicit(&token_ptr->is_cancelled, memory_order_acquire);
    if (!already_cancelled) {
        cb->next = token_ptr->head;
        if (token_ptr->head != NULL) {
            token_ptr->head->prev = cb;
        }
        token_ptr->head = cb;
        cb->active = true;
    } else {
        cb->claimed = true;
        cb->refcount = 1;   /* the token's list never took it */
    }
    pthread_mutex_unlock(&token_ptr->lock);

    if (already_cancelled) {
        callback(context_ptr);
    }
    return cb;
}

bool
UfsrvCancellationTokenUnregister(void *handle)
{
    if (unlikely(handle == NULL)) {
        return false;
    }

    struct UfsrvCancellationCallback *cb = handle;
    bool won = false;

    pthread_mutex_lock(&cb->token->lock);
    if (cb->active) {
        cb->active = false;
        cb->claimed = true;
        if (cb->prev != NULL) {
            cb->prev->next = cb->next;
        } else {
            cb->token->head = cb->next;
        }
        if (cb->next != NULL) {
            cb->next->prev = cb->prev;
        }
        cb->prev = NULL;
        cb->next = NULL;
        --cb->refcount;   /* the token's list reference */
        won = true;
    }
    bool last = (--cb->refcount == 0);
    pthread_mutex_unlock(&cb->token->lock);

    if (last) {
        sNodeFree(cb);
    }
    return won;
}

void
UfsrvCancellationTokenCancel(UfsrvCancellationToken *token_ptr)
{
    if (unlikely(token_ptr == NULL)) {
        return;
    }

    pthread_mutex_lock(&token_ptr->lock);
    if (atomic_exchange_explicit(&token_ptr->is_cancelled, true, memory_order_acq_rel)) {
        pthread_mutex_unlock(&token_ptr->lock);
        return;
    }

    struct UfsrvCancellationCallback *node = token_ptr->head;
    token_ptr->head = NULL;

    /* The whole list is detached here; each node's list reference is dropped only
     * after its callback has run, so Unregister cannot free a running node. */
    for (struct UfsrvCancellationCallback *walk = node; walk != NULL; walk = walk->next) {
        walk->active = false;
        walk->claimed = true;
        walk->prev = NULL;
    }
    pthread_mutex_unlock(&token_ptr->lock);

    while (node != NULL) {
        struct UfsrvCancellationCallback *next = node->next;

        node->fn(node->context);
        node->next = NULL;
        sNodeDropRef(node);
        node = next;
    }
}

bool
UfsrvCancellationTokenIsCancelled(const UfsrvCancellationToken *token_ptr)
{
    if (unlikely(token_ptr == NULL)) {
        return false;
    }
    return atomic_load_explicit(&token_ptr->is_cancelled, memory_order_acquire);
}

void
UfsrvCancellationTokenDestroy(UfsrvCancellationToken *token_ptr)
{
    if (unlikely(token_ptr == NULL)) {
        return;
    }
    sTokenRelease(token_ptr);
}
