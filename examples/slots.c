/**
 * @file slots.c
 * @brief Example: live number "slots", each driven by a coroutine.
 *
 * Two execution models, selected by a command-line switch:
 *
 *   cooperative (default) — ONE scheduler runs NUM_SLOTS coroutines that cooperate:
 *     each generates one number, re-submits itself and yields, so the single worker
 *     round-robins between them (M:N).
 *
 *   --parallel           — NUM_SLOTS schedulers, each running one coroutine that loops
 *     with usleep (1:1 threads).
 *
 * No global memory: each slot's state is passed to its coroutine by context.
 *
 * Build and run:  ./slots [--parallel]
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <uflib/standard_defs.h>
#include <uflib/standard_c_includes.h>

#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>

#include <stdatomic.h>
#include <time.h>

#define NUM_SLOTS 5

/*!
 * One display slot: per-slot state shared between its coroutine (writer) and the UI
 * thread (reader). `seed` is coroutine-private; the rest are atomics.
 */
struct Slot {
    unsigned    seed;    /*!< Per-slot PRNG state (coroutine-only). */
    atomic_int  number;  /*!< Latest value (read by the UI). */
    atomic_int  tick;    /*!< Generation count. */
    atomic_bool running; /*!< Stop flag. */
};

/*!
 * @brief Cooperative coroutine: generate one number per turn, then yield.
 */
static void
sCooperativeEntry(void)
{
    struct Slot *slot = UfsrvCoroutineGetArg();

    while (atomic_load_explicit(&slot->running, memory_order_acquire)) {
        slot->seed = slot->seed * 1103515245u + 12345u;   /* simple LCG */
        atomic_store_explicit(&slot->number, (int)((slot->seed >> 16) & 0x7fffu),
                              memory_order_relaxed);
        atomic_fetch_add_explicit(&slot->tick, 1, memory_order_relaxed);

        /* Cooperative: hand control back to the worker, ask to be resumed again. */
        UfsrvCoroutineSubmitResume(UfsrvCoroutineCurrentScheduler(), UfsrvCoroutineCurrent());
        UfsrvCoroutineYield();
    }

    UfsrvCoroutineExit();
}

/*!
 * @brief Parallel coroutine: loop on its own worker, sleeping between numbers.
 */
static void
sParallelEntry(void)
{
    struct Slot *slot = UfsrvCoroutineGetArg();

    while (atomic_load_explicit(&slot->running, memory_order_acquire)) {
        slot->seed = slot->seed * 1103515245u + 12345u;
        atomic_store_explicit(&slot->number, (int)((slot->seed >> 16) & 0x7fffu),
                              memory_order_relaxed);
        atomic_fetch_add_explicit(&slot->tick, 1, memory_order_relaxed);
        usleep(30000u + ((slot->seed >> 20) & 0xffffu));  /* 30–95 ms of "work" */
    }

    UfsrvCoroutineExit();
}

/*!
 * @brief Redraw the slots in place (home the cursor and overwrite).
 */
static void
sRenderSlots(struct Slot *slots, size_t count)
{
    printf("\033[H");
    for (size_t i = 0; i < count; i++) {
        printf("Slot %zu: %5d   (tick %d)\n", i + 1,
               atomic_load_explicit(&slots[i].number, memory_order_relaxed),
               atomic_load_explicit(&slots[i].tick, memory_order_relaxed));
    }
    fflush(stdout);
}

static void
sInitSlots(struct Slot *slots, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        slots[i].seed = (unsigned)time(NULL) + (unsigned)i * 7919u;
        atomic_init(&slots[i].number, 0);
        atomic_init(&slots[i].tick, 0);
        atomic_init(&slots[i].running, true);
    }
}

static void
sStopSlots(struct Slot *slots, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        atomic_store_explicit(&slots[i].running, false, memory_order_release);
    }
}

int
main(int argc, char **argv)
{
    bool parallel = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--parallel") == 0) {
            parallel = true;
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("usage: %s [--parallel]\n", argv[0]);
            printf("  (default) cooperative: one scheduler, %d coroutines\n", NUM_SLOTS);
            printf("  --parallel:            %d schedulers, one coroutine each\n", NUM_SLOTS);
            return 0;
        }
    }

    struct Slot slots[NUM_SLOTS];
    sInitSlots(slots, NUM_SLOTS);

    if (parallel) {
        UfsrvScheduler *schedulers[NUM_SLOTS];

        for (int i = 0; i < NUM_SLOTS; i++) {
            schedulers[i] = UfsrvSchedulerCreate("slot");
            if (schedulers[i] == NULL || UfsrvSchedulerStart(schedulers[i]) != 0) {
                fprintf(stderr, "scheduler %d failed\n", i);
                return 1;
            }
            UfsrvCoroutineSpawn(schedulers[i], sParallelEntry, &slots[i]);
        }
        fprintf(stderr, "parallel: %d schedulers x 1 coroutine\n", NUM_SLOTS);

        printf("\033[2J");
        for (int frame = 0; frame < 80; frame++) {
            sRenderSlots(slots, NUM_SLOTS);
            usleep(50000);
        }

        sStopSlots(slots, NUM_SLOTS);
        for (int i = 0; i < NUM_SLOTS; i++) {
            UfsrvSchedulerStop(schedulers[i]);
            UfsrvSchedulerJoin(schedulers[i]);
            UfsrvSchedulerDestroy(schedulers[i]);
        }
    } else {
        UfsrvScheduler *scheduler = UfsrvSchedulerCreate("slots");
        if (scheduler == NULL || UfsrvSchedulerStart(scheduler) != 0) {
            fprintf(stderr, "scheduler failed\n");
            return 1;
        }
        for (int i = 0; i < NUM_SLOTS; i++) {
            UfsrvCoroutineSpawn(scheduler, sCooperativeEntry, &slots[i]);
        }
        fprintf(stderr, "cooperative: 1 scheduler x %d coroutines\n", NUM_SLOTS);

        printf("\033[2J");
        for (int frame = 0; frame < 80; frame++) {
            sRenderSlots(slots, NUM_SLOTS);
            usleep(50000);
        }

        sStopSlots(slots, NUM_SLOTS);
        UfsrvSchedulerStop(scheduler);
        UfsrvSchedulerJoin(scheduler);
        UfsrvSchedulerDestroy(scheduler);
    }

    printf("\nDone.\n");
    return 0;
}
