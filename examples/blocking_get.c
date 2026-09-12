/**
 * @file blocking_get.c
 * @brief Example: a plain thread blocks on UfsrvFutureGet for a background result.
 *
 * Demonstrates the auto-dispatching get(): a coroutine computes a "slow" result on a
 * worker thread, while the main thread — which is *not* a coroutine — calls
 * UfsrvFutureGet and blocks (eventfd) until the result is ready. Inside a coroutine the
 * same call would suspend cooperatively; here it takes the new blocking path.
 *
 * Build and run:  ./blocking_get
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>
#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>

/*! Compute context passed to the worker coroutine (no global memory). */
struct ComputeContext {
    UfsrvPromise *promise;
    int           base;
};

/*! Coroutine entry: do some simulated work on the worker, then fulfil the promise. */
static void
sComputeEntry(void)
{
    struct ComputeContext *ctx = UfsrvCoroutineGetArg();

    for (int i = 0; i < 5; i++) {
        usleep(100000);   /* simulate ~100 ms of work per step */
    }

    int *result = malloc(sizeof(*result));
    if (result != NULL) {
        *result = ctx->base * 2;
    }
    UfsrvPromiseSetValue(ctx->promise, result, free);
    free(ctx);

    UfsrvCoroutineExit();
}

int
main(void)
{
    UfsrvScheduler *scheduler = UfsrvSchedulerCreate("compute");
    if (scheduler == NULL || UfsrvSchedulerStart(scheduler) != 0) {
        fprintf(stderr, "scheduler start failed\n");
        return 1;
    }

    UfsrvFuture *future = NULL;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    if (promise == NULL) {
        fprintf(stderr, "promise create failed\n");
        return 1;
    }

    struct ComputeContext *ctx = malloc(sizeof(*ctx));
    ctx->promise = promise;
    ctx->base = 21;
    UfsrvCoroutineSpawn(scheduler, sComputeEntry, ctx);

    printf("Waiting for the result (blocking on a plain thread)...\n");
    UfsrvFutureResult r = UfsrvFutureGet(future);   /* blocks until ready */

    if (r.error == 0 && r.value != NULL) {
        printf("result = %d\n", *(int *)r.value);
    } else {
        printf("failed: %d\n", r.error);
    }

    UfsrvFutureRelease(future);
    UfsrvSchedulerStop(scheduler);
    UfsrvSchedulerJoin(scheduler);
    UfsrvSchedulerDestroy(scheduler);

    return 0;
}
