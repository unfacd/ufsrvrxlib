/**
 * @file ufsrv_coroutine.c
 * @brief Cooperative coroutines — thin wrapper over libaco (via uflib).
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

#include <libaco/aco.h>

#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>
#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>

/* Per-thread libaco state: the main co, one shared stack, and a one-time init flag. */
static __thread aco_t *t_main_co = NULL;
static __thread aco_share_stack_t *t_share_stack = NULL;
static __thread bool t_initialized = false;
static __thread UfsrvScheduler *t_current_scheduler = NULL;

/* pthread TLS key so per-thread coroutine state is torn down when a worker exits. */
static pthread_key_t t_thread_state_key;
static pthread_once_t t_thread_state_key_once = PTHREAD_ONCE_INIT;

/*!
 * @brief Free the calling thread's coroutine state (main co + shared stack).
 */
static void
sFreeThreadState(void)
{
    if (t_share_stack != NULL) {
        aco_share_stack_destroy(t_share_stack);
        t_share_stack = NULL;
    }
    if (t_main_co != NULL) {
        aco_destroy(t_main_co);
        t_main_co = NULL;
    }
    t_initialized = false;
}

/*!
 * @brief pthread TLS destructor — frees the thread's coroutine state at exit.
 *
 * @param[in] arg  Unused.
 */
static void
sThreadStateDestructor(void *arg)
{
    (void)arg;
    sFreeThreadState();
}

static void
sMakeThreadStateKey(void)
{
    pthread_key_create(&t_thread_state_key, sThreadStateDestructor);
}

/*!
 * @brief Initialise libaco for the calling thread on first use.
 *
 * Creates the thread's main coroutine (aco_create with a NULL main_co) and one
 * shared stack. Coroutines created on this thread yield back to this main co.
 * Registers a pthread TLS destructor so the state is freed at thread exit.
 */
static void
sEnsureThreadInit(void)
{
    if (t_initialized) {
        return;
    }
    pthread_once(&t_thread_state_key_once, sMakeThreadStateKey);
    pthread_setspecific(t_thread_state_key, (void *)1);   /* non-NULL → destructor at exit */

    aco_thread_init(NULL);
    t_main_co = aco_create(NULL, NULL, 0, NULL, NULL);   /* the thread's main co */
    t_share_stack = aco_share_stack_new(0);              /* default 2 MiB + guard page */
    t_initialized = true;
}

UfsrvCoroutine *
UfsrvCoroutineCreate(UfsrvCoroutineEntry entry, void *arg_ptr)
{
    if (entry == NULL) {
        return NULL;
    }

    sEnsureThreadInit();
    return aco_create(t_main_co, t_share_stack, 0, entry, arg_ptr);
}

void
UfsrvCoroutineResume(UfsrvCoroutine *coroutine_ptr)
{
    if (coroutine_ptr == NULL) {
        return;
    }

    sEnsureThreadInit();
    aco_resume(coroutine_ptr);

    if (coroutine_ptr->is_end) {
        aco_destroy(coroutine_ptr);
    }
}

void
UfsrvCoroutineYield(void)
{
    aco_yield();
}

UfsrvCoroutine *
UfsrvCoroutineCurrent(void)
{
    return aco_get_co();
}

void *
UfsrvCoroutineGetArg(void)
{
    return aco_get_arg();
}

UfsrvScheduler *
UfsrvCoroutineCurrentScheduler(void)
{
    return t_current_scheduler;
}

void
UfsrvCoroutineExit(void)
{
    aco_exit();
}

void
UfsrvCoroutineThreadCleanup(void)
{
    if (!t_initialized) {
        return;
    }
    sFreeThreadState();
}

/*! Resume-job context: the target scheduler and coroutine. */
struct UfsrvResumeContext {
    UfsrvScheduler *scheduler;
    UfsrvCoroutine *coroutine;
};

/*!
 * @brief Scheduler job callback — resume a suspended coroutine.
 *
 * @param[in] context_ptr  Pointer to a UfsrvResumeContext (freed here).
 */
static void
sResumeJob(void *context_ptr)
{
    struct UfsrvResumeContext *ctx = context_ptr;

    t_current_scheduler = ctx->scheduler;
    UfsrvCoroutineResume(ctx->coroutine);
    free(ctx);
}

void
UfsrvCoroutineSubmitResume(UfsrvScheduler *scheduler_ptr, UfsrvCoroutine *coroutine_ptr)
{
    if (scheduler_ptr == NULL || coroutine_ptr == NULL) {
        return;
    }

    struct UfsrvResumeContext *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    ctx->scheduler = scheduler_ptr;
    ctx->coroutine = coroutine_ptr;

    if (!UfsrvSchedulerSubmit(scheduler_ptr, sResumeJob, ctx)) {
        free(ctx);
    }
}

/*! Spawn-job context: the target scheduler, entry, and its argument. */
struct UfsrvSpawnContext {
    UfsrvScheduler     *scheduler;
    UfsrvCoroutineEntry entry;
    void               *arg_ptr;
};

/*!
 * @brief Scheduler job callback — create a coroutine on the worker and resume it.
 *
 * @param[in] context_ptr  Pointer to a UfsrvSpawnContext (freed here).
 */
static void
sSpawnJob(void *context_ptr)
{
    struct UfsrvSpawnContext *ctx = context_ptr;

    t_current_scheduler = ctx->scheduler;

    UfsrvCoroutine *co = UfsrvCoroutineCreate(ctx->entry, ctx->arg_ptr);
    if (co != NULL) {
        UfsrvCoroutineResume(co);   /* runs to first yield; destroys if it exits */
    }
    free(ctx);
}

void
UfsrvCoroutineSpawn(UfsrvScheduler *scheduler_ptr, UfsrvCoroutineEntry entry, void *arg_ptr)
{
    if (scheduler_ptr == NULL || entry == NULL) {
        return;
    }

    struct UfsrvSpawnContext *ctx = malloc(sizeof(*ctx));
    if (ctx == NULL) {
        return;
    }
    ctx->scheduler = scheduler_ptr;
    ctx->entry = entry;
    ctx->arg_ptr = arg_ptr;

    if (!UfsrvSchedulerSubmit(scheduler_ptr, sSpawnJob, ctx)) {
        free(ctx);
    }
}
