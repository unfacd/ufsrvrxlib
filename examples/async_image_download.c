/**
 * @file async_image_download.c
 * @brief Example: asynchronously download a random image while showing a progress bar.
 *
 * Demonstrates the full async stack working together:
 *   - UfsrvScheduler      (L4): a worker thread runs the download.
 *   - UfsrvCoroutine      (L3): the download runs as a coroutine on a worker thread.
 *   - UfsrvFuture/Promise (L2): the result is a future; a `then` signals completion.
 *
 * The download is simulated (no real network I/O) to keep the example focused on the
 * library's async primitives. Build and run:  ./async_image_download
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>
#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>

#include <stdatomic.h>
#include <time.h>

#define CHUNK_BYTES (64u * 1024u)   /* simulated chunk size */

static atomic_int s_progress = 0;   /* 0..100 */
static atomic_int s_done = 0;

/*! The "downloaded image" — its size and a fake URL, for the demo. */
struct Image {
    size_t size;
    char   url[64];
};

static void
sFreeImage(void *ptr)
{
    free(ptr);
}

/*! Download context passed to the coroutine. */
struct DownloadContext {
    size_t       total_size;
    UfsrvPromise *promise;
};

/*!
 * @brief Coroutine entry: simulate a chunked download, updating progress.
 */
static void
sDownloadEntry(void)
{
    struct DownloadContext *ctx = UfsrvCoroutineGetArg();

    size_t remaining = ctx->total_size;
    while (remaining > 0) {
        size_t chunk = remaining < CHUNK_BYTES ? remaining : CHUNK_BYTES;
        usleep(20000);   /* simulate network latency */
        remaining -= chunk;
        atomic_store_explicit(&s_progress,
                              (int)((ctx->total_size - remaining) * 100 / ctx->total_size),
                              memory_order_relaxed);
        /* usleep simulates blocking network I/O — the main thread stays free. */
    }

    struct Image *image = malloc(sizeof(*image));
    image->size = ctx->total_size;
    snprintf(image->url, sizeof(image->url), "https://picsum.photos/seed/%u",
             (unsigned)(ctx->total_size / 1024u));
    UfsrvPromiseSetValue(ctx->promise, image, sFreeImage);
    free(ctx);

    UfsrvCoroutineExit();
}

/*!
 * @brief Future continuation: signal completion and print the result.
 */
static void
sOnDone(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    (void)context_ptr;

    if (result_ptr->error == 0) {
        struct Image *image = result_ptr->value;
        printf("\rDownloaded %s (%zu bytes)\n", image->url, image->size);
    } else {
        printf("\rDownload failed: %d\n", result_ptr->error);
    }
    atomic_store_explicit(&s_done, 1, memory_order_release);
}

/*!
 * @brief Render an ASCII progress bar at a given percentage.
 */
static void
sShowProgress(int pct)
{
    const int width = 40;
    int filled = width * pct / 100;

    printf("\r[");
    for (int i = 0; i < width; i++) {
        putchar(i < filled ? '=' : ' ');
    }
    printf("] %3d%%", pct);
    fflush(stdout);
}

int
main(void)
{
    /* Pick a random image size (1..4 MiB) for the demo. */
    srand((unsigned)time(NULL));
    size_t total_size = (size_t)(1u + (unsigned)rand() % 4u) * 1024u * 1024u;

    UfsrvScheduler *scheduler = UfsrvSchedulerCreate("downloader");
    if (scheduler == NULL) {
        fprintf(stderr, "scheduler create failed\n");
        return 1;
    }
    if (UfsrvSchedulerStart(scheduler) != 0) {
        fprintf(stderr, "scheduler start failed\n");
        return 1;
    }

    UfsrvFuture *future = NULL;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    if (promise == NULL) {
        fprintf(stderr, "promise create failed\n");
        return 1;
    }

    UfsrvFutureThen(future, sOnDone, NULL);

    struct DownloadContext *ctx = malloc(sizeof(*ctx));
    ctx->total_size = total_size;
    ctx->promise = promise;

    printf("Downloading a random image (%zu bytes)...\n", total_size);
    UfsrvCoroutineSpawn(scheduler, sDownloadEntry, ctx);

    /* Render the progress bar until the future completes. */
    while (!atomic_load_explicit(&s_done, memory_order_acquire)) {
        int pct = atomic_load_explicit(&s_progress, memory_order_relaxed);
        sShowProgress(pct);
        usleep(50000);
    }
    sShowProgress(100);
    printf("\n");

    UfsrvFutureRelease(future);
    UfsrvSchedulerStop(scheduler);
    UfsrvSchedulerJoin(scheduler);
    UfsrvSchedulerDestroy(scheduler);

    return 0;
}
