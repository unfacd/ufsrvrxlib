/**
 * @file cancellation_chain.c
 * @brief Example: cancellation propagates through a flatMap chain.
 *
 * A two-stage pipeline — "fetch a user", then flatMap to "fetch their orders" — has a
 * cancellation token attached to the source future. Cancelling the token aborts the
 * whole chain: the source, the flatMap's output, and the flatMap's *inner* future all
 * complete with ECANCELED. The inner "orders" future is deliberately left pending to
 * show that cancel — not its producer — finishes it.
 *
 * This demonstrates UfsrvFutureAttachCancellation + UfsrvFutureFlatMap propagation
 * (option 2), the feature added in v0.16.0.
 *
 * Build and run:  ./cancellation_chain
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>
#include <ufsrvrxlib/ufsrv_cancellation/ufsrv_cancellation.h>

#include <stdatomic.h>

/*! The "orders" stage — its promise is held (never completed) to model a slow query. */
struct OrdersContext {
    UfsrvPromise *inner_promise;
    UfsrvFuture  *inner_future;
};

/*! flatMap mapper: user id → orders future (async). The result is left pending here. */
static UfsrvFuture *
sFetchOrders(const void *value_ptr, void *context_ptr)
{
    (void)value_ptr;
    struct OrdersContext *ctx = context_ptr;

    UfsrvFuture *orders = NULL;
    UfsrvPromise *orders_promise = UfsrvPromiseCreate(&orders);

    ctx->inner_promise = orders_promise;   /* keep it — the "query" never completes */
    ctx->inner_future = orders;

    return orders;   /* the flatMap takes ownership of this reference */
}

/*! Terminal continuation: report how the pipeline finished. */
static void
sOnDone(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    atomic_int *done = context_ptr;

    if (result_ptr->error == ECANCELED) {
        printf("pipeline cancelled (ECANCELED)\n");
    } else if (result_ptr->error == 0) {
        printf("pipeline done: %d orders\n", *(int *)result_ptr->value);
    } else {
        printf("pipeline failed: %d\n", result_ptr->error);
    }
    atomic_store_explicit(done, 1, memory_order_release);
}

int
main(void)
{
    UfsrvFuture *src = NULL;
    UfsrvPromise *src_promise = UfsrvPromiseCreate(&src);

    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    UfsrvFutureAttachCancellation(src, token);

    struct OrdersContext orders_ctx = { NULL, NULL };
    UfsrvFuture *orders = UfsrvFutureFlatMap(src, sFetchOrders, &orders_ctx);

    atomic_int done = 0;
    UfsrvFutureThen(orders, sOnDone, &done);

    /* Stage 1 completes: the "user" is fetched, kicking off the "orders" query. */
    int *user_id = malloc(sizeof(*user_id));
    *user_id = 42;
    UfsrvPromiseSetValue(src_promise, user_id, free);

    printf("orders query pending before cancel: %s\n",
           UfsrvFutureIsReady(orders_ctx.inner_future) ? "no" : "yes");

    /* Cancel the token — the whole chain (src → flatMap output → inner future) aborts. */
    UfsrvCancellationTokenCancel(token);

    while (!atomic_load_explicit(&done, memory_order_acquire)) {
        usleep(10000);
    }

    printf("inner future ready after cancel: %s\n",
           UfsrvFutureIsReady(orders_ctx.inner_future) ? "yes" : "no");

    /* Cleanup: the inner future's promise owns the only remaining reference to it. */
    UfsrvFutureRelease(orders);
    UfsrvFutureRelease(src);
    UfsrvPromiseDestroy(orders_ctx.inner_promise);
    UfsrvCancellationTokenDestroy(token);

    return 0;
}
