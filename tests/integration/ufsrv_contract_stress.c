/**
 * @file ufsrv_contract_stress.c
 * @brief Standalone multi-threaded contract stress harness for the
 *        future/promise, cancellation, coroutine and scheduler layers.
 *
 * Not CTest-registered — run manually, e.g.:
 *   ./ufsrv_contract_stress --wait-threads 4 --wait-iters 1500
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <ufsrvrxlib/ufsrv_cancellation/ufsrv_cancellation.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>
#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>
#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define LEDGER_SLOTS   (UINT64_C(1) << 20)
#define TRACKED_MAGIC  UINT64_C(0x5A5AC0DE5A5AC0DE)
#define SPIN_LIMIT_NS  (UINT64_C(60) * 1000 * 1000 * 1000)

enum ValueOrigin {
    ORIGIN_WAIT = 0,
    ORIGIN_COMPLETION,
    ORIGIN_PARTNER,
    ORIGIN_MAP,
    ORIGIN_ZIP,
    ORIGIN_RECOVERY,
    ORIGIN_COUNT
};

static const char *const s_origin_names[ORIGIN_COUNT] = {
    "wait", "completion", "partner", "map", "zip", "recovery"
};

struct TrackedValue {
    uint64_t magic;
    uint64_t id;
    uint64_t tag;
    int      origin;
};

static _Atomic(long) s_live_by_origin[ORIGIN_COUNT];

static _Atomic(uint8_t)  s_ledger[LEDGER_SLOTS];
static _Atomic(uint64_t) s_values_alloc;
static _Atomic(uint64_t) s_values_free;
static _Atomic(uint64_t) s_ledger_reused;
static _Atomic(uint64_t) s_magic_bad;
static _Atomic(uint64_t) s_id_next;
static _Atomic(uint64_t) s_id_overflow;

static uint64_t
sMonotonicNs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *
sValueNew(int origin)
{
    struct TrackedValue *value_ptr = malloc(sizeof(*value_ptr));
    if (value_ptr == NULL) {
        return NULL;
    }
    value_ptr->magic = TRACKED_MAGIC;
    value_ptr->origin = origin;
    value_ptr->tag = 0;
    value_ptr->id = atomic_fetch_add_explicit(&s_id_next, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_live_by_origin[origin], 1, memory_order_relaxed);
    if (value_ptr->id < LEDGER_SLOTS) {
        atomic_store_explicit(&s_ledger[value_ptr->id], 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&s_id_overflow, 1, memory_order_relaxed);
    }
    atomic_fetch_add_explicit(&s_values_alloc, 1, memory_order_relaxed);
    return value_ptr;
}

static void
sValueFree(void *ptr)
{
    struct TrackedValue *value_ptr = ptr;
    if (value_ptr == NULL) {
        return;
    }
    if (value_ptr->magic != TRACKED_MAGIC) {
        atomic_fetch_add_explicit(&s_magic_bad, 1, memory_order_relaxed);
        return;
    }
    value_ptr->magic = 0;
    if (value_ptr->id < LEDGER_SLOTS) {
        uint8_t previous = atomic_exchange_explicit(&s_ledger[value_ptr->id], 2, memory_order_relaxed);
        if (previous != 1) {
            atomic_fetch_add_explicit(&s_ledger_reused, 1, memory_order_relaxed);
        }
    }
    atomic_fetch_sub_explicit(&s_live_by_origin[value_ptr->origin], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_values_free, 1, memory_order_relaxed);
    free(value_ptr);
}

static uint64_t
sLedgerLive(void)
{
    return atomic_load(&s_values_alloc) - atomic_load(&s_values_free);
}

static void
sValueInspect(const void *ptr)
{
    const struct TrackedValue *value_ptr = ptr;
    if (value_ptr == NULL || value_ptr->magic != TRACKED_MAGIC) {
        atomic_fetch_add_explicit(&s_magic_bad, 1, memory_order_relaxed);
    }
}

struct WaitTicket {
    _Atomic(int)            refcount;
    _Atomic(int)            entered;
    uint64_t                issued_id;
    UfsrvFuture            *future;
    UfsrvCancellationToken *token;
};

static _Atomic(long) s_tickets_live;
static _Atomic(long) s_wait_delivered;
static _Atomic(long) s_wait_cancelled;
static _Atomic(long) s_wait_mismatched;
static _Atomic(long) s_wait_unexpected;
static _Atomic(long) s_wait_issued_value;
static _Atomic(long) s_wait_issued_cancel;
static _Atomic(long) s_wait_not_ready;
static _Atomic(long) s_wait_handshake;

static void
sTicketRetain(struct WaitTicket *ticket)
{
    atomic_fetch_add_explicit(&ticket->refcount, 1, memory_order_relaxed);
}

static void
sTicketRelease(struct WaitTicket *ticket)
{
    if (atomic_fetch_sub_explicit(&ticket->refcount, 1, memory_order_acq_rel) != 1) {
        return;
    }
    UfsrvFutureRelease(ticket->future);
    UfsrvCancellationTokenDestroy(ticket->token);
    free(ticket);
    atomic_fetch_sub_explicit(&s_tickets_live, 1, memory_order_relaxed);
}

static void
sWaitConsumerEntry(void)
{
    struct WaitTicket *ticket = UfsrvCoroutineGetArg();

    if (!UfsrvCoroutineIsInCoroutine() || UfsrvCoroutineCurrentScheduler() == NULL) {
        atomic_fetch_add_explicit(&s_wait_unexpected, 1, memory_order_relaxed);
    }

    if (!UfsrvFutureIsReady(ticket->future)) {
        atomic_fetch_add_explicit(&s_wait_not_ready, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&ticket->entered, 1, memory_order_release);

    UfsrvFutureResult result = UfsrvFutureGetWithCancellation(ticket->future, ticket->token);

    if (result.error == ECANCELED) {
        atomic_fetch_add_explicit(&s_wait_cancelled, 1, memory_order_relaxed);
    } else if (result.error == 0 && result.value != NULL) {
        sValueInspect(result.value);
        const struct TrackedValue *value_ptr = result.value;
        if (value_ptr->magic == TRACKED_MAGIC && value_ptr->id != ticket->issued_id) {
            atomic_fetch_add_explicit(&s_wait_mismatched, 1, memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&s_wait_delivered, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&s_wait_unexpected, 1, memory_order_relaxed);
    }

    sTicketRelease(ticket);
    UfsrvCoroutineExit();
}

struct WaitArgs {
    UfsrvScheduler *scheduler;
    long            iterations;
    unsigned int    seed;
};

static void *
sWaitProducerMain(void *arg_ptr)
{
    struct WaitArgs *args = arg_ptr;

    for (long i = 0; i < args->iterations; i++) {
        UfsrvFuture *future_ptr = NULL;
        UfsrvPromise *promise_ptr = UfsrvPromiseCreate(&future_ptr);
        if (promise_ptr == NULL) {
            break;
        }

        UfsrvCancellationToken *token_ptr = UfsrvCancellationTokenCreate();
        if (token_ptr == NULL) {
            UfsrvPromiseDestroy(promise_ptr);
            UfsrvFutureRelease(future_ptr);
            break;
        }
        (void)UfsrvFutureAttachCancellation(future_ptr, token_ptr);

        struct WaitTicket *ticket = calloc(1, sizeof(*ticket));
        if (ticket == NULL) {
            UfsrvCancellationTokenDestroy(token_ptr);
            UfsrvPromiseDestroy(promise_ptr);
            UfsrvFutureRelease(future_ptr);
            break;
        }
        atomic_init(&ticket->refcount, 1);
        ticket->future = future_ptr;
        ticket->token = token_ptr;
        atomic_fetch_add_explicit(&s_tickets_live, 1, memory_order_relaxed);

        sTicketRetain(ticket);
        UfsrvCoroutineSpawn(args->scheduler, sWaitConsumerEntry, ticket);

        if ((rand_r(&args->seed) % 4u) == 0u) {
            uint64_t deadline = sMonotonicNs() + UINT64_C(2000000);
            while (atomic_load_explicit(&ticket->entered, memory_order_acquire) == 0) {
                if (sMonotonicNs() >= deadline) {
                    break;
                }
                sched_yield();
            }
            atomic_fetch_add_explicit(&s_wait_handshake, 1, memory_order_relaxed);
        }

        if ((rand_r(&args->seed) % 3u) == 0u) {
            UfsrvCancellationTokenCancel(token_ptr);
            atomic_fetch_add_explicit(&s_wait_issued_cancel, 1, memory_order_relaxed);
        } else {
            struct TrackedValue *value_ptr = sValueNew(ORIGIN_WAIT);
            ticket->issued_id = (value_ptr != NULL) ? value_ptr->id : UINT64_MAX;
            UfsrvPromiseSetValue(promise_ptr, value_ptr, sValueFree);
            atomic_fetch_add_explicit(&s_wait_issued_value, 1, memory_order_relaxed);
        }

        UfsrvPromiseDestroy(promise_ptr);
        sTicketRelease(ticket);
    }
    return NULL;
}

static bool
sWaitForTickets(uint64_t limit_ns)
{
    uint64_t deadline = sMonotonicNs() + limit_ns;
    while (atomic_load_explicit(&s_tickets_live, memory_order_acquire) != 0) {
        if (sMonotonicNs() >= deadline) {
            return false;
        }
        sched_yield();
    }
    return true;
}

#define REG_TOKEN_COUNT   8
#define REG_SLOTS_PER_TOK 64

struct RegSlot {
    _Atomic(int) ran;
    _Atomic(int) released;
    _Atomic(int) claimed;
};

static struct RegSlot s_reg_slots[REG_TOKEN_COUNT][REG_SLOTS_PER_TOK];

static void
sRegCallback(void *context_ptr)
{
    struct RegSlot *slot_ptr = context_ptr;
    atomic_fetch_add_explicit(&slot_ptr->ran, 1, memory_order_relaxed);
}

static void
sRegRelease(void *context_ptr)
{
    struct RegSlot *slot_ptr = context_ptr;
    atomic_fetch_add_explicit(&slot_ptr->released, 1, memory_order_relaxed);
}

struct RegArgs {
    UfsrvCancellationToken *token;
    struct RegSlot         *slots;
    _Atomic(bool)          *stop;
    long                    count;
    unsigned int            seed;
};

static void *
sRegistrantMain(void *arg_ptr)
{
    struct RegArgs *args = arg_ptr;

    for (long i = 0; i < args->count; i++) {
        void *handle = UfsrvCancellationTokenRegisterEx(args->token, sRegCallback, &args->slots[i], sRegRelease);
        if (handle == NULL) {
            continue;
        }
        if ((rand_r(&args->seed) % 4u) == 0u) {
            struct timespec pause = { .tv_sec = 0, .tv_nsec = 20000 };
            nanosleep(&pause, NULL);
        }
        if (UfsrvCancellationTokenUnregister(handle)) {
            atomic_store_explicit(&args->slots[i].claimed, 1, memory_order_relaxed);
        }
        handle = NULL;
    }
    return NULL;
}

static void *
sRegCancellerMain(void *arg_ptr)
{
    struct RegArgs *args = arg_ptr;
    while (!atomic_load_explicit(args->stop, memory_order_acquire)) {
        UfsrvCancellationTokenCancel(args->token);
        sched_yield();
    }
    UfsrvCancellationTokenCancel(args->token);
    return NULL;
}

struct ChainIteration {
    _Atomic(int) done;
    _Atomic(int) recovered;
    uint64_t     tag;
};

static _Atomic(long)     s_chain_terminal;
static _Atomic(long)     s_chain_failed;
static _Atomic(long)     s_chain_unfinished;
static _Atomic(long)     s_chain_recovered;
static _Atomic(long)     s_chain_issued_error;
static _Atomic(long)     s_chain_issued_cancel;
static _Atomic(long)     s_chain_mismatched;
static _Atomic(uint64_t) s_chain_tag_next;

static void *
sValueStamp(void *value_ptr, uint64_t tag)
{
    struct TrackedValue *tracked_ptr = value_ptr;
    if (tracked_ptr != NULL && tracked_ptr->magic == TRACKED_MAGIC) {
        tracked_ptr->tag = tag;
    }
    return value_ptr;
}

static void *
sMapFn(const void *value_ptr, void *context_ptr)
{
    const struct ChainIteration *iteration_ptr = context_ptr;
    sValueInspect(value_ptr);
    return sValueStamp(sValueNew(ORIGIN_MAP), iteration_ptr->tag);
}

static void *
sZipFn(const void *value_a_ptr, const void *value_b_ptr, void *context_ptr)
{
    const struct ChainIteration *iteration_ptr = context_ptr;
    sValueInspect(value_a_ptr);
    sValueInspect(value_b_ptr);
    return sValueStamp(sValueNew(ORIGIN_ZIP), iteration_ptr->tag);
}

static void
sActionFn(const void *value_ptr, void *context_ptr)
{
    (void)context_ptr;
    sValueInspect(value_ptr);
}

static UfsrvFuture *
sRecoverFn(int error, void *context_ptr)
{
    struct ChainIteration *iteration_ptr = context_ptr;
    (void)error;
    atomic_fetch_add_explicit(&iteration_ptr->recovered, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&s_chain_recovered, 1, memory_order_relaxed);
    return UfsrvFutureFromValue(sValueStamp(sValueNew(ORIGIN_RECOVERY), iteration_ptr->tag), sValueFree);
}

static void
sTerminalFn(UfsrvFutureResult *result_ptr, void *context_ptr)
{
    struct ChainIteration *iteration_ptr = context_ptr;

    if (result_ptr->error == 0) {
        sValueInspect(result_ptr->value);
        const struct TrackedValue *value_ptr = result_ptr->value;
        if (value_ptr != NULL && value_ptr->magic == TRACKED_MAGIC && value_ptr->tag != iteration_ptr->tag) {
            atomic_fetch_add_explicit(&s_chain_mismatched, 1, memory_order_relaxed);
        }
        atomic_fetch_add_explicit(&s_chain_terminal, 1, memory_order_relaxed);
    } else {
        atomic_fetch_add_explicit(&s_chain_failed, 1, memory_order_relaxed);
    }
    atomic_store_explicit(&iteration_ptr->done, 1, memory_order_release);
}

enum ChainOutcome {
    CHAIN_OUTCOME_VALUE  = 0,
    CHAIN_OUTCOME_ERROR  = 1,
    CHAIN_OUTCOME_CANCEL = 2
};

struct ChainCompletion {
    UfsrvPromise           *promise;
    UfsrvCancellationToken *token;
    int                     outcome;
};

static void
sChainCompletionJob(void *context_ptr)
{
    struct ChainCompletion *completion_ptr = context_ptr;

    switch (completion_ptr->outcome) {
    case CHAIN_OUTCOME_CANCEL:
        UfsrvCancellationTokenCancel(completion_ptr->token);
        break;
    case CHAIN_OUTCOME_ERROR:
        UfsrvPromiseSetError(completion_ptr->promise, ETIMEDOUT);
        break;
    default:
        UfsrvPromiseSetValue(completion_ptr->promise, sValueNew(ORIGIN_COMPLETION), sValueFree);
        break;
    }
    UfsrvPromiseDestroy(completion_ptr->promise);
    free(completion_ptr);
}

struct ChainArgs {
    UfsrvSchedulerPool *pool;
    long                iterations;
    unsigned int        seed;
};

static void *
sChainBuilderMain(void *arg_ptr)
{
    struct ChainArgs *args = arg_ptr;

    for (long i = 0; i < args->iterations; i++) {
        struct ChainIteration iteration;
        atomic_init(&iteration.done, 0);
        atomic_init(&iteration.recovered, 0);
        iteration.tag = atomic_fetch_add_explicit(&s_chain_tag_next, 1, memory_order_relaxed);

        UfsrvFuture *future_ptr = NULL;
        UfsrvPromise *promise_ptr = UfsrvPromiseCreate(&future_ptr);
        if (promise_ptr == NULL) {
            break;
        }

        UfsrvCancellationToken *token_ptr = UfsrvCancellationTokenCreate();
        if (token_ptr == NULL) {
            UfsrvPromiseDestroy(promise_ptr);
            UfsrvFutureRelease(future_ptr);
            break;
        }
        (void)UfsrvFutureAttachCancellation(future_ptr, token_ptr);

        struct ChainCompletion *completion_ptr = calloc(1, sizeof(*completion_ptr));
        if (completion_ptr == NULL) {
            UfsrvCancellationTokenDestroy(token_ptr);
            UfsrvPromiseDestroy(promise_ptr);
            UfsrvFutureRelease(future_ptr);
            break;
        }
        completion_ptr->promise = promise_ptr;
        completion_ptr->token = token_ptr;

        unsigned int roll = rand_r(&args->seed) % 4u;
        if (roll == 0u) {
            completion_ptr->outcome = CHAIN_OUTCOME_CANCEL;
            atomic_fetch_add_explicit(&s_chain_issued_cancel, 1, memory_order_relaxed);
        } else if (roll == 1u) {
            completion_ptr->outcome = CHAIN_OUTCOME_ERROR;
            atomic_fetch_add_explicit(&s_chain_issued_error, 1, memory_order_relaxed);
        }
        UfsrvSchedulerPoolSubmit(args->pool, sChainCompletionJob, completion_ptr);

        UfsrvFuture *mapped_ptr = UfsrvFutureMapEx(future_ptr, sMapFn, &iteration, sValueFree);
        UfsrvFuture *partner_ptr = UfsrvFutureFromValue(sValueNew(ORIGIN_PARTNER), sValueFree);
        UfsrvFuture *zipped_ptr = UfsrvFutureZipEx(mapped_ptr, partner_ptr, sZipFn, &iteration, sValueFree);
        UfsrvFuture *side_ptr = UfsrvFutureDoOnSuccess(zipped_ptr, sActionFn, NULL);
        UfsrvFuture *recovered_ptr = UfsrvFutureOnError(side_ptr, sRecoverFn, &iteration);

        if (recovered_ptr != NULL) {
            (void)UfsrvFutureThen(recovered_ptr, sTerminalFn, &iteration);
        } else {
            atomic_store_explicit(&iteration.done, 1, memory_order_release);
            atomic_fetch_add_explicit(&s_chain_failed, 1, memory_order_relaxed);
        }

        uint64_t deadline = sMonotonicNs() + SPIN_LIMIT_NS;
        while (atomic_load_explicit(&iteration.done, memory_order_acquire) == 0) {
            if (sMonotonicNs() >= deadline) {
                atomic_fetch_add_explicit(&s_chain_unfinished, 1, memory_order_relaxed);
                break;
            }
            sched_yield();
        }

        UfsrvFutureRelease(mapped_ptr);
        UfsrvFutureRelease(partner_ptr);
        UfsrvFutureRelease(zipped_ptr);
        UfsrvFutureRelease(side_ptr);
        UfsrvFutureRelease(recovered_ptr);
        UfsrvCancellationTokenDestroy(token_ptr);
        UfsrvFutureRelease(future_ptr);
    }
    return NULL;
}

struct Engine {
    long        wait_threads;
    long        wait_iters;
    long        chain_threads;
    long        chain_iters;
    unsigned    seed;
};

static void
sUsage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [--wait-threads N] [--wait-iters N]\n"
            "          [--chain-threads N] [--chain-iters N] [--seed N]\n"
            "  --wait-threads N   coroutine wait/cancel producers (default 4)\n"
            "  --wait-iters N     iterations per producer        (default 1500)\n"
            "  --chain-threads N  combinator builders            (default 4)\n"
            "  --chain-iters N    iterations per builder         (default 400)\n"
            "  --seed N           RNG seed                       (default 1)\n",
            prog);
}

static int
sRunWaitEngine(const struct Engine *engine)
{
    long threads = engine->wait_threads;
    int failures = 0;

    UfsrvScheduler **schedulers = calloc((size_t)threads, sizeof(*schedulers));
    pthread_t *tids = calloc((size_t)threads, sizeof(*tids));
    struct WaitArgs *args = calloc((size_t)threads, sizeof(*args));
    if (schedulers == NULL || tids == NULL || args == NULL) {
        free(schedulers);
        free(tids);
        free(args);
        return 1;
    }

    for (long t = 0; t < threads; t++) {
        schedulers[t] = UfsrvSchedulerCreate("contract-wait");
        if (schedulers[t] == NULL || UfsrvSchedulerStart(schedulers[t]) != 0) {
            fprintf(stderr, "[wait] scheduler start failed at %ld\n", t);
            for (long k = 0; k < t; k++) {
                UfsrvSchedulerDestroy(schedulers[k]);
            }
            free(schedulers);
            free(tids);
            free(args);
            return 1;
        }
    }

    for (long t = 0; t < threads; t++) {
        args[t].scheduler = schedulers[t];
        args[t].iterations = engine->wait_iters;
        args[t].seed = engine->seed + (unsigned)t * 2654435761u + 1u;
        pthread_create(&tids[t], NULL, sWaitProducerMain, &args[t]);
    }
    for (long t = 0; t < threads; t++) {
        pthread_join(tids[t], NULL);
    }

    if (!sWaitForTickets(SPIN_LIMIT_NS)) {
        fprintf(stderr, "[wait] FAIL: %ld ticket(s) never released — coroutine(s) stranded\n",
                atomic_load_explicit(&s_tickets_live, memory_order_acquire));
        failures++;
    }

    for (long t = 0; t < threads; t++) {
        UfsrvSchedulerStop(schedulers[t]);
        UfsrvSchedulerJoin(schedulers[t]);
        UfsrvSchedulerDestroy(schedulers[t]);
    }
    free(schedulers);
    free(tids);
    free(args);

    long issued_value = atomic_load(&s_wait_issued_value);
    long issued_cancel = atomic_load(&s_wait_issued_cancel);
    long delivered = atomic_load(&s_wait_delivered);
    long cancelled = atomic_load(&s_wait_cancelled);
    long unexpected = atomic_load(&s_wait_unexpected);

    printf("[wait]  issued: value=%ld cancel=%ld  observed: delivered=%ld cancelled=%ld unexpected=%ld\n",
           issued_value, issued_cancel, delivered, cancelled, unexpected);
    printf("[wait]  handshake=%ld slow-path=%ld\n",
           atomic_load(&s_wait_handshake), atomic_load(&s_wait_not_ready));

    if (unexpected != 0) {
        fprintf(stderr, "[wait] FAIL: %ld unexpected result(s)\n", unexpected);
        failures++;
    }
    if (delivered != issued_value || cancelled != issued_cancel) {
        fprintf(stderr, "[wait] FAIL: delivered=%ld for %ld value issue(s), cancelled=%ld for %ld cancel issue(s)\n",
                delivered, issued_value, cancelled, issued_cancel);
        failures++;
    }
    long mismatched = atomic_load(&s_wait_mismatched);
    if (mismatched != 0) {
        fprintf(stderr, "[wait] FAIL: %ld waiter(s) received a value issued to another iteration\n", mismatched);
        failures++;
    }
    if (delivered + cancelled > 0 && (delivered == 0 || cancelled == 0)) {
        fprintf(stderr, "[wait] WARN: only one completion path exercised\n");
    }
    return failures;
}

static int
sRunRegistryEngine(unsigned seed)
{
    int failures = 0;

    UfsrvCancellationToken *tokens[REG_TOKEN_COUNT];
    _Atomic(bool) stop[REG_TOKEN_COUNT];
    struct RegArgs args[REG_TOKEN_COUNT];
    pthread_t registrants[REG_TOKEN_COUNT];
    pthread_t cancellers[REG_TOKEN_COUNT];

    for (int t = 0; t < REG_TOKEN_COUNT; t++) {
        tokens[t] = UfsrvCancellationTokenCreate();
        if (tokens[t] == NULL) {
            fprintf(stderr, "[registry] token create failed\n");
            return 1;
        }
        atomic_init(&stop[t], false);
        if ((t % 4) == 0) {
            UfsrvCancellationTokenCancel(tokens[t]);
        }
        args[t].token = tokens[t];
        args[t].slots = s_reg_slots[t];
        args[t].stop = &stop[t];
        args[t].count = REG_SLOTS_PER_TOK;
        args[t].seed = seed + (unsigned)t * 2246822519u + 7u;

        pthread_create(&cancellers[t], NULL, sRegCancellerMain, &args[t]);
        pthread_create(&registrants[t], NULL, sRegistrantMain, &args[t]);
    }

    for (int t = 0; t < REG_TOKEN_COUNT; t++) {
        pthread_join(registrants[t], NULL);
    }
    for (int t = 0; t < REG_TOKEN_COUNT; t++) {
        atomic_store_explicit(&stop[t], true, memory_order_release);
    }
    for (int t = 0; t < REG_TOKEN_COUNT; t++) {
        pthread_join(cancellers[t], NULL);
        UfsrvCancellationTokenCancel(tokens[t]);
    }

    long claimed_total = 0;
    long ran_total = 0;
    long bad_sum = 0;
    long unreleased = 0;
    int shown = 0;

    for (int t = 0; t < REG_TOKEN_COUNT; t++) {
        for (long i = 0; i < REG_SLOTS_PER_TOK; i++) {
            int ran = atomic_load_explicit(&s_reg_slots[t][i].ran, memory_order_relaxed);
            int claimed = atomic_load_explicit(&s_reg_slots[t][i].claimed, memory_order_relaxed);
            int released = atomic_load_explicit(&s_reg_slots[t][i].released, memory_order_relaxed);

            if (ran + claimed != 1) {
                bad_sum++;
                if (shown++ < 5) {
                    fprintf(stderr, "[registry] FAIL: slot %d/%ld ran=%d claimed=%d\n", t, i, ran, claimed);
                }
            }
            if (released != 1) {
                unreleased++;
            }
            claimed_total += claimed;
            ran_total += ran;
        }
    }

    for (int t = 0; t < REG_TOKEN_COUNT; t++) {
        UfsrvCancellationTokenDestroy(tokens[t]);
    }

    long unreleased_after = 0;
    for (int t = 0; t < REG_TOKEN_COUNT; t++) {
        for (long i = 0; i < REG_SLOTS_PER_TOK; i++) {
            if (atomic_load_explicit(&s_reg_slots[t][i].released, memory_order_relaxed) != 1) {
                unreleased_after++;
            }
        }
    }

    if (bad_sum != 0) {
        fprintf(stderr, "[registry] FAIL: %ld slot(s) with ran+claimed != 1\n", bad_sum);
        failures++;
    }
    if (unreleased != 0 || unreleased_after != 0) {
        fprintf(stderr, "[registry] FAIL: %ld node(s) not reclaimed before destroy, %ld after\n",
                unreleased, unreleased_after);
        failures++;
    }

    printf("[registry] registrations=%d ran=%ld claimed=%ld reclaimed=%ld/%d\n",
           REG_TOKEN_COUNT * REG_SLOTS_PER_TOK, ran_total, claimed_total,
           REG_TOKEN_COUNT * REG_SLOTS_PER_TOK - unreleased_after, REG_TOKEN_COUNT * REG_SLOTS_PER_TOK);
    if (ran_total == 0 || claimed_total == 0) {
        fprintf(stderr, "[registry] WARN: only one neutralisation path exercised\n");
    }
    return failures;
}

static int
sRunChainEngine(const struct Engine *engine)
{
    long threads = engine->chain_threads;
    int failures = 0;

    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate((int)threads, "contract-chain");
    if (pool == NULL || UfsrvSchedulerPoolStart(pool) != 0) {
        fprintf(stderr, "[chain] pool start failed\n");
        UfsrvSchedulerPoolDestroy(pool);
        return 1;
    }

    pthread_t *tids = calloc((size_t)threads, sizeof(*tids));
    struct ChainArgs *args = calloc((size_t)threads, sizeof(*args));
    if (tids == NULL || args == NULL) {
        free(tids);
        free(args);
        UfsrvSchedulerPoolStop(pool);
        UfsrvSchedulerPoolJoin(pool);
        UfsrvSchedulerPoolDestroy(pool);
        return 1;
    }

    for (long t = 0; t < threads; t++) {
        args[t].pool = pool;
        args[t].iterations = engine->chain_iters;
        args[t].seed = engine->seed + (unsigned)t * 40503u + 13u;
        pthread_create(&tids[t], NULL, sChainBuilderMain, &args[t]);
    }
    for (long t = 0; t < threads; t++) {
        pthread_join(tids[t], NULL);
    }

    UfsrvSchedulerPoolStop(pool);
    UfsrvSchedulerPoolJoin(pool);
    UfsrvSchedulerPoolDestroy(pool);
    free(tids);
    free(args);

    long terminal = atomic_load(&s_chain_terminal);
    long failed = atomic_load(&s_chain_failed);
    long unfinished = atomic_load(&s_chain_unfinished);
    long recovered = atomic_load(&s_chain_recovered);
    long issued_error = atomic_load(&s_chain_issued_error);
    long issued_cancel = atomic_load(&s_chain_issued_cancel);
    long expected = threads * engine->chain_iters;

    printf("[chain] chains=%ld terminal=%ld failed=%ld unfinished=%ld recovered=%ld (error=%ld cancel=%ld)\n",
           expected, terminal, failed, unfinished, recovered, issued_error, issued_cancel);

    if (unfinished != 0) {
        fprintf(stderr, "[chain] FAIL: %ld chain(s) never reached a terminal\n", unfinished);
        failures++;
    }
    if (terminal != expected || failed != 0) {
        fprintf(stderr, "[chain] FAIL: terminal=%ld failed=%ld for %ld chain(s)\n", terminal, failed, expected);
        failures++;
    }
    long mismatched = atomic_load(&s_chain_mismatched);
    if (mismatched != 0) {
        fprintf(stderr, "[chain] FAIL: %ld chain(s) received a value produced by another iteration\n", mismatched);
        failures++;
    }
    if (recovered != issued_error + issued_cancel) {
        fprintf(stderr, "[chain] FAIL: %ld recovery(ies) for %ld error completion(s)\n",
                recovered, issued_error + issued_cancel);
        failures++;
    }
    return failures;
}

int
main(int argc, char **argv)
{
    struct Engine engine = {
        .wait_threads = 4,
        .wait_iters = 1500,
        .chain_threads = 4,
        .chain_iters = 400,
        .seed = 1,
    };

    static const struct option longopts[] = {
        { "wait-threads",  required_argument, NULL, 'w' },
        { "wait-iters",    required_argument, NULL, 'i' },
        { "chain-threads", required_argument, NULL, 'c' },
        { "chain-iters",   required_argument, NULL, 'n' },
        { "seed",          required_argument, NULL, 's' },
        { "help",          no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    int c;
    while ((c = getopt_long(argc, argv, "w:i:c:n:s:h", longopts, NULL)) != -1) {
        switch (c) {
        case 'w': engine.wait_threads  = strtol(optarg, NULL, 10); break;
        case 'i': engine.wait_iters    = strtol(optarg, NULL, 10); break;
        case 'c': engine.chain_threads = strtol(optarg, NULL, 10); break;
        case 'n': engine.chain_iters   = strtol(optarg, NULL, 10); break;
        case 's': engine.seed          = (unsigned)strtoul(optarg, NULL, 10); break;
        case 'h': sUsage(argv[0]); return 0;
        default:  sUsage(argv[0]); return 2;
        }
    }

    if (engine.wait_threads <= 0 || engine.wait_iters <= 0 ||
        engine.chain_threads <= 0 || engine.chain_iters <= 0) {
        sUsage(argv[0]);
        return 2;
    }

    uint64_t value_budget = (uint64_t)engine.wait_threads * (uint64_t)engine.wait_iters +
                            (uint64_t)engine.chain_threads * (uint64_t)engine.chain_iters * 4u;
    if (value_budget >= LEDGER_SLOTS) {
        fprintf(stderr, "value budget %" PRIu64 " exceeds ledger %" PRIu64 "\n", value_budget, (uint64_t)LEDGER_SLOTS);
        return 2;
    }

    printf("contract stress: wait=%ldx%ld chain=%ldx%ld seed=%u\n",
           engine.wait_threads, engine.wait_iters, engine.chain_threads, engine.chain_iters, engine.seed);

    int failures = 0;
    failures += sRunRegistryEngine(engine.seed);
    printf("[ledger] after registry: live=%" PRIu64 "\n", sLedgerLive());
    failures += sRunWaitEngine(&engine);
    printf("[ledger] after wait: live=%" PRIu64 "\n", sLedgerLive());
    failures += sRunChainEngine(&engine);
    printf("[ledger] after chain: live=%" PRIu64 "\n", sLedgerLive());

    uint64_t alloc = atomic_load(&s_values_alloc);
    uint64_t freed = atomic_load(&s_values_free);
    uint64_t reused = atomic_load(&s_ledger_reused);
    uint64_t magic_bad = atomic_load(&s_magic_bad);
    uint64_t overflow = atomic_load(&s_id_overflow);

    for (int i = 0; i < ORIGIN_COUNT; i++) {
        long live = atomic_load(&s_live_by_origin[i]);
        if (live != 0) {
            fprintf(stderr, "[ledger] live origin=%s count=%ld\n", s_origin_names[i], live);
        }
    }
    printf("[ledger] alloc=%" PRIu64 " free=%" PRIu64 " reused-slot=%" PRIu64
           " bad-magic=%" PRIu64 " id-overflow=%" PRIu64 "\n",
           alloc, freed, reused, magic_bad, overflow);

    if (alloc != freed) {
        fprintf(stderr, "[ledger] FAIL: %" PRIu64 " value(s) leaked or double-freed\n",
                alloc > freed ? alloc - freed : freed - alloc);
        failures++;
    }
    if (reused != 0 || magic_bad != 0 || overflow != 0) {
        fprintf(stderr, "[ledger] FAIL: value lifecycle anomaly\n");
        failures++;
    }

    if (failures != 0) {
        printf("RESULT: FAIL (%d check group(s))\n", failures);
        return 1;
    }
    printf("RESULT: PASS\n");
    return 0;
}
