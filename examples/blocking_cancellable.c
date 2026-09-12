/**
 * @file blocking_cancellable.c
 * @brief Example: a plain thread blocks with a cancellation timeout.
 *
 * A slow "download" runs as a coroutine on a worker; the main thread (a plain thread)
 * blocks in UfsrvFutureGetWithCancellation with a timeout token. A timeout thread
 * cancels the token after ~300 ms. If the download finishes first the value is
 * delivered; if the timeout fires first the get returns ECANCELED. Demonstrates the
 * blocking + cancellable wait added in v0.17.0.
 *
 * Build and run:  ./blocking_cancellable [--late]
 *   (default) timeout cancels → ECANCELED
 *   --late    download finishes → value delivered
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <pthread.h>

#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>
#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>
#include <ufsrvrxlib/ufsrv_cancellation/ufsrv_cancellation.h>

/*! Download context passed to the worker coroutine. */
struct DownloadContext {
    UfsrvPromise *promise;
};

/*! Coroutine entry: simulate a slow download (10 chunks × 100 ms). */
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

/*! Timeout thread: cancel the token after a fixed delay. */
static void *
sTimeoutThread(void *arg)
{
    UfsrvCancellationToken *token = arg;
    usleep(300000);
    UfsrvCancellationTokenCancel(token);
    return NULL;
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
            printf("  (default) timeout cancels → ECANCELED\n");
            printf("  --late    download finishes → value delivered\n");
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

    struct DownloadContext *ctx = malloc(sizeof(*ctx));
    ctx->promise = promise;
    UfsrvCoroutineSpawn(scheduler, sDownloadEntry, ctx);

    /* A timeout thread cancels the token after ~300 ms. */
    pthread_t timeout;
    if (!late) {
        pthread_create(&timeout, NULL, sTimeoutThread, token);
    }

    printf("Downloading (300 ms timeout)...\n");
    UfsrvFutureResult r = UfsrvFutureGetWithCancellation(future, token);   /* blocks */

    if (!late) {
        pthread_join(timeout, NULL);
    }

    if (r.error == ECANCELED) {
        printf("timed out (ECANCELED)\n");
    } else if (r.error == 0) {
        printf("downloaded %s\n", (char *)r.value);
    } else {
        printf("failed: %d\n", r.error);
    }

    UfsrvFutureRelease(future);
    UfsrvCancellationTokenDestroy(token);
    UfsrvSchedulerStop(scheduler);
    UfsrvSchedulerJoin(scheduler);
    UfsrvSchedulerDestroy(scheduler);

    return 0;
}
