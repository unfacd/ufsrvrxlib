/**
 * @file ufsrv_scheduler_stress.c
 * @brief Standalone scheduler stress test: many producer threads submit jobs.
 *
 * Not CTest-registered — run manually, e.g.:
 *   ./ufsrv_scheduler_stress --threads 8 --jobs 1000000 --workers 4
 *
 * Verifies every accepted job runs exactly once (zero-error) and reports
 * submission throughput.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>

#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

/*! Counters shared across producers and workers. */
static atomic_ulong s_submitted = 0;
static atomic_ulong s_completed = 0;
static atomic_int   s_failed    = 0;

/*!
 * @brief Job callback — atomically count completions.
 *
 * @param[in] context_ptr  Unused.
 */
static void
sCountJob(void *context_ptr)
{
    (void)context_ptr;
    atomic_fetch_add_explicit(&s_completed, 1, memory_order_relaxed);
}

/*! Producer-thread arguments. */
struct ProducerArgs {
    UfsrvSchedulerPool *pool;
    long                jobs;
};

/*!
 * @brief Producer thread entry — submit `jobs` jobs.
 *
 * @param[in] arg_ptr  Pointer to a ProducerArgs.
 * @return Always NULL.
 */
static void *
sProducerMain(void *arg_ptr)
{
    struct ProducerArgs *args = arg_ptr;

    for (long i = 0; i < args->jobs; i++) {
        if (UfsrvSchedulerPoolSubmit(args->pool, sCountJob, NULL)) {
            atomic_fetch_add_explicit(&s_submitted, 1, memory_order_relaxed);
        } else {
            atomic_fetch_add_explicit(&s_failed, 1, memory_order_relaxed);
        }
    }
    return NULL;
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

/*!
 * @brief Print usage to stderr.
 *
 * @param[in] prog  Program name (argv[0]).
 */
static void
sUsage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--threads N] [--jobs N] [--workers N]\n"
            "  --threads N   producer threads (default 8)\n"
            "  --jobs N      total jobs (default 1000000)\n"
            "  --workers N   pool workers (default 4)\n",
            prog);
}

/*!
 * @brief Stress-test entry point.
 */
int
main(int argc, char **argv)
{
    long threads = 8;
    long jobs    = 1000000;
    long workers = 4;

    static const struct option longopts[] = {
        { "threads", required_argument, NULL, 't' },
        { "jobs",    required_argument, NULL, 'j' },
        { "workers", required_argument, NULL, 'w' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "t:j:w:h", longopts, NULL)) != -1) {
        switch (c) {
        case 't': threads = strtol(optarg, NULL, 10); break;
        case 'j': jobs    = strtol(optarg, NULL, 10); break;
        case 'w': workers = strtol(optarg, NULL, 10); break;
        case 'h': sUsage(argv[0]); return 0;
        default:  sUsage(argv[0]); return 2;
        }
    }

    if (threads <= 0 || jobs <= 0 || workers <= 0) {
        fprintf(stderr, "threads/jobs/workers must be > 0\n");
        return 2;
    }

    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate((int)workers, "stress");
    if (pool == NULL) {
        fprintf(stderr, "pool create failed\n");
        return 1;
    }
    if (UfsrvSchedulerPoolStart(pool) != 0) {
        fprintf(stderr, "pool start failed\n");
        UfsrvSchedulerPoolDestroy(pool);
        return 1;
    }

    struct ProducerArgs *args = calloc((size_t)threads, sizeof(*args));
    pthread_t *tids = calloc((size_t)threads, sizeof(*tids));
    if (args == NULL || tids == NULL) {
        fprintf(stderr, "allocation failed\n");
        UfsrvSchedulerPoolStop(pool);
        UfsrvSchedulerPoolJoin(pool);
        UfsrvSchedulerPoolDestroy(pool);
        free(args);
        free(tids);
        return 1;
    }

    long per_thread = jobs / threads;
    long remainder  = jobs % threads;

    double t0 = sNowSeconds();

    for (long t = 0; t < threads; t++) {
        args[t].pool = pool;
        args[t].jobs = per_thread + (t < remainder ? 1 : 0);
        if (pthread_create(&tids[t], NULL, sProducerMain, &args[t]) != 0) {
            fprintf(stderr, "pthread_create failed at index %ld\n", t);
            for (long k = 0; k < t; k++) {
                pthread_join(tids[k], NULL);
            }
            UfsrvSchedulerPoolStop(pool);
            UfsrvSchedulerPoolJoin(pool);
            UfsrvSchedulerPoolDestroy(pool);
            free(args);
            free(tids);
            return 1;
        }
    }

    for (long t = 0; t < threads; t++) {
        pthread_join(tids[t], NULL);
    }

    UfsrvSchedulerPoolStop(pool);
    UfsrvSchedulerPoolJoin(pool);
    UfsrvSchedulerPoolDestroy(pool);

    double t1 = sNowSeconds();

    free(args);
    free(tids);

    unsigned long submitted = atomic_load_explicit(&s_submitted, memory_order_relaxed);
    unsigned long completed = atomic_load_explicit(&s_completed, memory_order_relaxed);
    int failed = atomic_load_explicit(&s_failed, memory_order_relaxed);

    double seconds = t1 - t0;
    double rate    = seconds > 0.0 ? (double)completed / seconds : 0.0;

    printf("threads=%ld jobs=%ld workers=%ld\n", threads, jobs, workers);
    printf("submitted=%lu completed=%lu failed=%d\n", submitted, completed, failed);
    printf("elapsed=%.3fs throughput=%.0f jobs/s\n", seconds, rate);

    if (failed != 0 || submitted != (unsigned long)jobs || completed != (unsigned long)jobs) {
        fprintf(stderr, "FAIL: submitted/completed mismatch or submit failures\n");
        return 1;
    }
    printf("OK: all %lu jobs completed exactly once\n", completed);
    return 0;
}
