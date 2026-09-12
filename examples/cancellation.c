/**
 * @file cancellation.c
 * @brief Example: cooperative cancellation via a token attached to a future.
 *
 * A "download" coroutine runs a long simulated transfer. The main thread attaches a
 * cancellation token to the future and, after a short timeout, cancels it — the future
 * completes with ECANCELED instead of the download result. This shows
 * UfsrvFutureAttachCancellation + UfsrvCancellationTokenCancel (option-2 propagation):
 * cancelling the token completes the future exactly once, racing the promise.
 *
 * Build and run:  ./cancellation [--late]
 *   (default) cancels mid-download → ECANCELED
 *   --late    lets the download finish → value delivered
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>
#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>
#include <ufsrvrxlib/ufsrv_cancellation/ufsrv_cancellation.h>

#include <stdatomic.h>

/*! Download context passed to the worker coroutine. */
struct DownloadContext {
    UfsrvPromise *promise;
};

/*! Coroutine entry: simulate a long download (10 chunks × 100 ms). */
static void
sDownloadEntry(void)
{
    struct DownloadContext *ctx = UfsrvCoroutineGetArg();

    for (int i = 0; i < 10; i++) {
        usleep(100000);
    }

    char *url = malloc(32);
    if (url != NULL) {
        strcpy(url, "https://example.com/image.jpg");
    }
    UfsrvPromiseSetValue(ctx->promise, url, free);
    free(ctx);

    UfsrvCoroutineExit();
}

/*! Completion continuation: report the outcome and signal the main thread. */
static void
sOnDone(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    atomic_int *done = context_ptr;

    if (result_ptr->error == ECANCELED) {
        printf("\nDownload cancelled.\n");
    } else if (result_ptr->error == 0) {
        printf("\nDownloaded %s\n", (char *)result_ptr->value);
    } else {
        printf("\nDownload failed: %d\n", result_ptr->error);
    }
    atomic_store_explicit(done, 1, memory_order_release);
}

int
main(int argc, char **argv)
{
    bool late = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--late") == 0) {
            late = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--late]\n", argv[0]);
            printf("  (default) cancel mid-download → ECANCELED\n");
            printf("  --late    let the download finish → value delivered\n");
            return 0;
        }
    }

    UfsrvScheduler *scheduler = UfsrvSchedulerCreate("dl");
    if (scheduler == NULL || UfsrvSchedulerStart(scheduler) != 0) {
        fprintf(stderr, "scheduler start failed\n");
        return 1;
    }

    UfsrvFuture *future = NULL;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    if (promise == NULL || token == NULL) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    UfsrvFutureAttachCancellation(future, token);

    atomic_int done = 0;
    UfsrvFutureThen(future, sOnDone, &done);

    struct DownloadContext *ctx = malloc(sizeof(*ctx));
    ctx->promise = promise;
    UfsrvCoroutineSpawn(scheduler, sDownloadEntry, ctx);

    printf("Downloading (cancel in ~300 ms)...\n");
    usleep(300000);
    if (!late) {
        UfsrvCancellationTokenCancel(token);   /* completes the future with ECANCELED */
    }

    /* Wait for the download to finish, or be cancelled. */
    while (!atomic_load_explicit(&done, memory_order_acquire)) {
        usleep(10000);
    }

    UfsrvFutureRelease(future);
    UfsrvCancellationTokenDestroy(token);
    UfsrvSchedulerStop(scheduler);
    UfsrvSchedulerJoin(scheduler);
    UfsrvSchedulerDestroy(scheduler);

    return 0;
}
