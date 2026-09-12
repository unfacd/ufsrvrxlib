/**
 * @file ufsrv_layers_e2e.c
 * @brief Standalone multi-threaded test spanning scheduler, coroutine, and future.
 *
 * Spawns coroutines across several worker schedulers. Each coroutine creates and
 * completes a future (running a `then` continuation), then yields and re-submits
 * itself, resuming once before it exits. Verifies every future and every coroutine
 * completed exactly once (zero-error).
 *
 * Not CTest-registered — run manually, e.g.:
 *   ./ufsrv_layers_e2e --workers 4 --coroutines 1000
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

static atomic_int g_future_completed = 0;
static atomic_int g_coroutine_finished = 0;

/*!
 * @brief Future continuation — count completions.
 *
 * @param[in] result_ptr   Result (borrowed, unused).
 * @param[in] context_ptr  Unused.
 */
static void
sOnFutureDone(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    (void)result_ptr;
    (void)context_ptr;
    atomic_fetch_add_explicit(&g_future_completed, 1, memory_order_relaxed);
}

/*! Per-coroutine context. */
struct CoroutineContext {
    UfsrvScheduler *scheduler;  /*!< The worker this coroutine is pinned to. */
};

/*!
 * @brief Coroutine entry — exercise the future layer, then yield/resume.
 */
static void
sCoroutineEntry(void)
{
    struct CoroutineContext *ctx = UfsrvCoroutineGetArg();

    /* Future layer: create, complete, and observe a future. */
    UfsrvFuture *future = NULL;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    UfsrvFutureThen(future, sOnFutureDone, NULL);
    UfsrvPromiseSetValue(promise, NULL, NULL);   /* completes → runs sOnFutureDone */
    UfsrvFutureRelease(future);

    /* Coroutine layer: re-submit self to this worker, yield, then resume. */
    UfsrvCoroutineSubmitResume(ctx->scheduler, UfsrvCoroutineCurrent());
    UfsrvCoroutineYield();

    atomic_fetch_add_explicit(&g_coroutine_finished, 1, memory_order_relaxed);
    UfsrvCoroutineExit();
}

/*!
 * @brief Monotonic clock in seconds.
 *
 * @return Seconds since an arbitrary epoch (monotonic).
 */
static double
sNowSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int
main(int argc, char **argv)
{
    int workers = 4;
    int coroutines = 1000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--workers") == 0 && i + 1 < argc) {
            workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--coroutines") == 0 && i + 1 < argc) {
            coroutines = atoi(argv[++i]);
        }
    }
    if (workers <= 0 || coroutines <= 0) {
        fprintf(stderr, "usage: %s [--workers N] [--coroutines N]\n", argv[0]);
        return 2;
    }

    UfsrvScheduler **schedulers = calloc((size_t)workers, sizeof(*schedulers));
    struct CoroutineContext *contexts = calloc((size_t)coroutines, sizeof(*contexts));
    if (schedulers == NULL || contexts == NULL) {
        fprintf(stderr, "allocation failed\n");
        free(schedulers);
        free(contexts);
        return 1;
    }

    for (int i = 0; i < workers; i++) {
        schedulers[i] = UfsrvSchedulerCreate("e2e");
        if (schedulers[i] == NULL || UfsrvSchedulerStart(schedulers[i]) != 0) {
            fprintf(stderr, "scheduler %d failed\n", i);
            return 1;
        }
    }

    double t0 = sNowSeconds();

    for (int i = 0; i < coroutines; i++) {
        contexts[i].scheduler = schedulers[i % workers];
        UfsrvCoroutineSpawn(schedulers[i % workers], sCoroutineEntry, &contexts[i]);
    }

    /* Bounded wait for every coroutine to finish (up to ~10 s). */
    int deadline = 10000;
    while (atomic_load_explicit(&g_coroutine_finished, memory_order_relaxed) < coroutines
           && deadline-- > 0) {
        usleep(1000);
    }

    for (int i = 0; i < workers; i++) {
        UfsrvSchedulerStop(schedulers[i]);
        UfsrvSchedulerJoin(schedulers[i]);
        UfsrvSchedulerDestroy(schedulers[i]);
    }

    double t1 = sNowSeconds();
    free(schedulers);
    free(contexts);

    int future_done = atomic_load_explicit(&g_future_completed, memory_order_relaxed);
    int coro_done = atomic_load_explicit(&g_coroutine_finished, memory_order_relaxed);

    printf("workers=%d coroutines=%d\n", workers, coroutines);
    printf("futures completed=%d coroutines finished=%d\n", future_done, coro_done);
    printf("elapsed=%.3fs\n", t1 - t0);

    if (future_done != coroutines || coro_done != coroutines) {
        fprintf(stderr, "FAIL: completion mismatch\n");
        return 1;
    }
    printf("OK: %d futures and %d coroutines completed exactly once\n", future_done, coro_done);
    return 0;
}
