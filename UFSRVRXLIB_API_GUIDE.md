# UFSRVRXLIB — Public API Guide

|                |                                                                    |
|----------------|--------------------------------------------------------------------|
| Service Name   | ufsrvrxlib                                                         |
| Library Version| 0.1.23 — `<ufsrvrxlib/version.h>` (`MAJOR.MINOR.PATCH`)            |
| Date Created   | 2026-10-03                                                         |
| Created By     | devops                                                             |
| Scope          | The complete exported surface: **62 functions** across 5 modules   |

This guide is the reference for everything a consumer of `libufsrvrxlib` may call. It
states each function's contract — semantics, ownership, threading and failure — as the
installed headers define it, and is self-contained: nothing below requires any other
document to be read.

## Table of Contents

1. [Scope and conventions](#1-scope-and-conventions)
2. [Module map](#2-module-map)
3. [Quick start](#3-quick-start)
4. [Scheduler (L4)](#4-scheduler-l4--ufsrv_schedulerh)
5. [Coroutines (L3)](#5-coroutines-l3--ufsrv_coroutineh)
6. [Futures and promises (L2)](#6-futures-and-promises-l2--ufsrv_futureh)
7. [Cancellation](#7-cancellation--ufsrv_cancellationh)
8. [Version](#8-version--versionh)
9. [Execution model](#9-execution-model)
10. [Ownership and lifetime](#10-ownership-and-lifetime)
11. [Error codes](#11-error-codes)
12. [Recipes](#12-recipes)
13. [Examples](#13-examples)
14. [Build and link](#14-build-and-link)
15. [Contract violations](#15-contract-violations)

---

## 1. Scope and conventions

```c
#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>       /* L4 */
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>       /* L3 */
#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>             /* L2 */
#include <ufsrvrxlib/ufsrv_cancellation/ufsrv_cancellation.h> /* cancellation */
#include <ufsrvrxlib/version.h>                               /* version */
```

Link with `-lufsrvrxlib`. The library is C17; every header is `extern "C"`-guarded and
consumable from C++.

| Convention | Meaning |
|---|---|
| `PUBLIC_API` | Export marker (`ufsrvrxlib_defs.h`). Applied to every function in this guide; a symbol without it is internal and not part of the contract. |
| `_ptr` suffix | Parameter is a pointer to an object; the object is opaque unless a type header defines it (`ufsrv_future_type.h`, `ufsrv_coroutine_type.h`, `ufsrv_scheduler_type.h`). |
| Opaque handles | `UfsrvFuture`, `UfsrvPromise`, `UfsrvCancellationToken`, `UfsrvScheduler`, `UfsrvSchedulerPool`, `UfsrvCoroutine`. Never dereference them; never `free()` them. |
| `bool` return | `true` = the operation succeeded; `false` = rejected (NULL argument, allocation failure, or a scheduler that is not running). |
| Pointer return | `NULL` = failure; the reason is not reported. |
| `_Ex` suffix | An additive variant that takes an explicit destructor for a value the library would otherwise release with `free`. |

## 2. Module map

The library is a four-layer stack; each layer depends only on the one beneath it.

| Layer | Module | Header | Functions |
|---|---|---|---|
| L4 | `ufsrv_scheduler` | `ufsrv_scheduler.h` | 17 |
| L3 | `ufsrv_coroutine` | `ufsrv_coroutine.h` | 11 |
| L2 | `ufsrv_future` | `ufsrv_future.h` | 23 |
| — | `ufsrv_cancellation` | `ufsrv_cancellation.h` | 7 |
| — | version | `version.h` | 4 |

| Module | Function inventory |
|---|---|
| Scheduler | `UfsrvSchedulerCreate` `Start` `Submit` `Stop` `Join` `Destroy` `IsWorkerThread` · `UfsrvSchedulerPoolCreate` `Start` `Submit` `SubmitTo` `Stop` `Join` `Destroy` `Size` · `DescribeScheduler` `ListSchedulerWorkers` |
| Coroutine | `UfsrvCoroutineCreate` `Resume` `Yield` `Current` `GetArg` `CurrentScheduler` `IsInCoroutine` `Exit` `ThreadCleanup` `SubmitResume` `Spawn` |
| Future/Promise | `UfsrvPromiseCreate` `SetValue` `SetError` `SetResult` `Destroy` · `UfsrvFutureThen` `IsReady` `Get` `GetWithCancellation` `AttachCancellation` `FromValue` `FromError` `Map` `MapEx` `FlatMap` `OnError` `DoOnSuccess` `All` `Zip` `ZipEx` `Any` `Retain` `Release` |
| Cancellation | `UfsrvCancellationTokenCreate` `Cancel` `IsCancelled` `Register` `RegisterEx` `Unregister` `Destroy` |
| Version | `UfsrvRxLibVersion` `VersionMajor` `VersionMinor` `VersionPatch` |

## 3. Quick start

A worker thread runs a coroutine; the coroutine fulfils a future; the main thread waits
for the result.

```c
#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>
#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>

struct Job { UfsrvPromise *promise; };

static void
sEntry(void)
{
    struct Job *job = UfsrvCoroutineGetArg();   /* the arg passed to Spawn */

    int *value = malloc(sizeof(*value));
    *value = 42;
    UfsrvPromiseSetValue(job->promise, value, free);  /* completes the future */
    UfsrvPromiseDestroy(job->promise);                /* Set* never frees it */
    UfsrvCoroutineExit();                             /* optional; return works too */
}

int
main(void)
{
    UfsrvScheduler *scheduler = UfsrvSchedulerCreate("worker");
    UfsrvSchedulerStart(scheduler);

    UfsrvFuture *future = NULL;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);

    struct Job job = { promise };
    UfsrvCoroutineSpawn(scheduler, sEntry, &job);

    UfsrvFutureResult result = UfsrvFutureGet(future);   /* main is a plain thread: blocks */
    if (result.error == 0) {
        printf("value = %d\n", *(int *)result.value);    /* borrowed: do not free */
    }

    UfsrvFutureRelease(future);
    UfsrvSchedulerStop(scheduler);
    UfsrvSchedulerJoin(scheduler);
    UfsrvSchedulerDestroy(scheduler);
    return 0;
}
```

---

## 4. Scheduler (L4) — `ufsrv_scheduler.h`

One `UfsrvScheduler` owns one OS thread and one lock-free MPSC queue. Any thread may
submit; the worker pops in submission order and runs each job's callback. A
`UfsrvSchedulerPool` is N schedulers with round-robin distribution.

| Function | Purpose | Returns |
|---|---|---|
| `UfsrvScheduler *UfsrvSchedulerCreate(const char *name_ptr)` | Create a single-worker scheduler, not started. `name_ptr` is copied (may be NULL). | New scheduler, or NULL on OOM |
| `int UfsrvSchedulerStart(UfsrvScheduler *)` | Create and start the worker thread. | 0, or −1 if already started / thread creation failed |
| `bool UfsrvSchedulerSubmit(UfsrvScheduler *, UfsrvSchedulerJobCallback, void *ctx)` | Enqueue a job. | `true` if accepted; `false` if not running or OOM |
| `void UfsrvSchedulerStop(UfsrvScheduler *)` | Ask the worker to stop. Does not wait. | — |
| `void UfsrvSchedulerJoin(UfsrvScheduler *)` | Block until the worker thread exits. | — |
| `void UfsrvSchedulerDestroy(UfsrvScheduler *)` | Stop if running, join, drain the queue, free. Queued jobs are freed **without** running their callbacks. NULL is a no-op. | — |
| `bool UfsrvSchedulerIsWorkerThread(const UfsrvScheduler *)` | Is the caller this scheduler's worker thread? | `true`/`false` |
| `UfsrvSchedulerPool *UfsrvSchedulerPoolCreate(int worker_count, const char *name_prefix_ptr)` | Create a pool of `worker_count` (> 0) schedulers, not started. | New pool, or NULL on invalid input / OOM |
| `int UfsrvSchedulerPoolStart(UfsrvSchedulerPool *)` | Start every worker. | 0, or −1 if any failed (already-started ones are stopped) |
| `bool UfsrvSchedulerPoolSubmit(UfsrvSchedulerPool *, UfsrvSchedulerJobCallback, void *)` | Submit round-robin. | `true` if accepted |
| `bool UfsrvSchedulerPoolSubmitTo(UfsrvSchedulerPool *, int worker_index, UfsrvSchedulerJobCallback, void *)` | Submit to a specific worker; falls back to round-robin if the index is out of range. | `true` if accepted |
| `void UfsrvSchedulerPoolStop(UfsrvSchedulerPool *)` | Signal all workers to stop. Does not wait. | — |
| `void UfsrvSchedulerPoolJoin(UfsrvSchedulerPool *)` | Wait for all workers to exit. | — |
| `void UfsrvSchedulerPoolDestroy(UfsrvSchedulerPool *)` | Stop if running, join, free the pool and its workers. NULL is a no-op. | — |
| `int UfsrvSchedulerPoolSize(const UfsrvSchedulerPool *)` | Worker count (0 if NULL). | count |

**Ordering.** The pool's round-robin counter is atomic; `SubmitTo` pins a job to one
worker, so two jobs submitted to the same worker run in submission order. Jobs on
different workers may run concurrently — the callback, not the scheduler, owns any
synchronisation the job needs.

**Lifecycle.** `Create → Start → Submit… → Stop → Join → Destroy` is the only valid
order. `Destroy` alone is sufficient (it performs the stop/join); `Stop`/`Join` are for
when the caller needs to drain before tearing down other state. A job submitted after
`Stop` may still run during the final drain.

### 4.1 Introspection

| Function | Purpose |
|---|---|
| `BufferDescriptor *DescribeScheduler(UfsrvSchedulerPool *pool, const char *worker_name, BufferDescriptor *provided)` | Render the pool and its workers as JSON (lifecycle flags, worker id, eventfd). `worker_name` filters to one exact name; NULL describes all. Pass a pre-initialised `BufferDescriptor`, or NULL to have one allocated (256-byte initial capacity). Returns the populated descriptor (the same pointer as `provided` when non-NULL), or NULL if a new one could not be allocated. |
| `CollectionDescriptor *ListSchedulerWorkers(UfsrvSchedulerPool *pool)` | Every worker name, in pool order, carved from a single allocation — descriptor, pointer array and strings. `collection_base_offset` is 0; read element *i* as `(const char *)workers->collection[i]`. Release the whole thing with one `free()`. NULL pool → NULL. |

```c
CollectionDescriptor *workers = ListSchedulerWorkers(pool);
BufferDescriptor bd; BufferDescriptorInit(&bd, 512);
for (size_t i = 0; i < workers->collection_sz; i++) {
    DescribeScheduler(pool, (const char *)workers->collection[i], &bd);
    printf("%s\n", bd.data);
}
BufferDescriptorRelease(&bd);
free(workers);
```

---

## 5. Coroutines (L3) — `ufsrv_coroutine.h`

A coroutine is a stackful unit of execution (libaco's `aco_t`, opaque). A body runs until
it yields or exits; a yielder is resumed later **on the thread that created it**.

| Function | Purpose | Returns |
|---|---|---|
| `UfsrvCoroutine *UfsrvCoroutineCreate(UfsrvCoroutineEntry entry, void *arg_ptr)` | Create a coroutine bound to the calling thread; it does not run until resumed. | New handle, or NULL on OOM |
| `void UfsrvCoroutineResume(UfsrvCoroutine *)` | Run until the next yield or exit. On exit the coroutine is destroyed and the handle is dead. NULL is a no-op. | — |
| `void UfsrvCoroutineYield(void)` | Suspend the current coroutine, returning to its resumer. | — |
| `UfsrvCoroutine *UfsrvCoroutineCurrent(void)` | The running coroutine, or the thread's main coroutine outside one. | handle |
| `void *UfsrvCoroutineGetArg(void)` | The `arg_ptr` passed to `Create`/`Spawn` for the current coroutine. | argument |
| `UfsrvScheduler *UfsrvCoroutineCurrentScheduler(void)` | The scheduler the current coroutine is pinned to. | scheduler, or NULL outside a scheduler-driven coroutine |
| `bool UfsrvCoroutineIsInCoroutine(void)` | Is the caller inside a coroutine (rather than the thread's main coroutine)? | `true`/`false` |
| `void UfsrvCoroutineExit(void)` | Terminate the current coroutine. Does not return. | — |
| `void UfsrvCoroutineThreadCleanup(void)` | Release the calling thread's coroutine state. Idempotent. | — |
| `bool UfsrvCoroutineSubmitResume(UfsrvScheduler *, UfsrvCoroutine *)` | Post a job that resumes a suspended coroutine on that scheduler's worker. | `true` if the job was accepted |
| `void UfsrvCoroutineSpawn(UfsrvScheduler *, UfsrvCoroutineEntry, void *arg_ptr)` | `Create` + first `Resume`, on a worker thread. | — |

**The entry point.** `UfsrvCoroutineEntry` is `void (*)(void)` — it takes no parameter;
the argument is read with `UfsrvCoroutineGetArg()`. A plain `return` from the entry
terminates the coroutine exactly as `UfsrvCoroutineExit()` does (an entry trampoline
reaches the exit path), so `UfsrvCoroutineExit()` is optional at the end of a body.

**Thread affinity.** A coroutine must be created on, and resumed on, the thread that owns
it. `UfsrvCoroutineSubmitResume` is how a coroutine that suspended *inside a worker* gets
back to that worker — it never migrates.

**Thread teardown.** `UfsrvCoroutineThreadCleanup` frees the thread's main coroutine and
shared stack, and reclaims any coroutine created on this thread but never resumed — the
handle of such a coroutine is invalid afterwards. Coroutines that have been resumed
(running or suspended) are **not** touched: they must have exited first. The state is
also released by a pthread TLS destructor when a thread exits without calling it, so a
worker thread's coroutine state does not outlive the thread.

**Argument lifetime.** `arg_ptr` is owned by the caller and must stay valid until the
coroutine has finished with it — including the case where the coroutine suspends on a
future and is resumed later.

---

## 6. Futures and promises (L2) — `ufsrv_future.h`

A `UfsrvPromise` is the single-shot producer; a `UfsrvFuture` is the consumer. When a
promise is fulfilled, the registered continuations run **on the completing thread**.
Every value carries its own destructor, so a value is released exactly once.

### 6.1 Producer side

| Function | Purpose | Returns |
|---|---|---|
| `UfsrvPromise *UfsrvPromiseCreate(UfsrvFuture **future_out)` | Create a linked pair. `future_out` receives the future with refcount 2 (promise + caller). | New promise, or NULL on NULL arg / OOM |
| `void UfsrvPromiseSetValue(UfsrvPromise *, void *value, void (*free_value)(void *))` | Fulfil with a success value (ownership transferred). First `Set*` wins. **Does not free the promise.** | — |
| `void UfsrvPromiseSetError(UfsrvPromise *, int error)` | Fulfil with a non-zero error code. Does not free the promise. | — |
| `void UfsrvPromiseSetResult(UfsrvPromise *, UfsrvFutureResult result)` | Fulfil with a full result (ownership transferred). Does not free the promise. | — |
| `void UfsrvPromiseDestroy(UfsrvPromise *)` | Release the promise. An **unfulfilled** promise completes its future with `EPIPE`. | — |

**Every promise must be destroyed**, fulfilled or not — `Set*` no longer frees it. The
future reference returned by `UfsrvPromiseCreate` is separate and must also be released.

`free_value` is the destructor for the success value: it is invoked when the last future
reference drops. NULL means the library never releases the value and the caller keeps
ownership of it.

### 6.2 Consumer side

| Function | Purpose | Returns |
|---|---|---|
| `bool UfsrvFutureThen(UfsrvFuture *, UfsrvFutureCallback, void *ctx)` | Register a terminal continuation. Runs immediately if the future is already ready, otherwise on completion. | `true`, or `false` on NULL args / OOM |
| `bool UfsrvFutureIsReady(const UfsrvFuture *)` | Has the future completed? NULL → `false`. | `true`/`false` |
| `UfsrvFutureResult UfsrvFutureGet(UfsrvFuture *)` | Wait for the result. See the execution model below. | **Borrowed** result |
| `UfsrvFutureResult UfsrvFutureGetWithCancellation(UfsrvFuture *, UfsrvCancellationToken *)` | As `Get`, plus a token: returns `ECANCELED` if the token is or becomes cancelled. NULL token → plain `Get`. | Borrowed result |
| `bool UfsrvFutureAttachCancellation(UfsrvFuture *, UfsrvCancellationToken *)` | Make the future complete with `ECANCELED` when the token fires, racing the promise (exactly one wins). At most one token per future. | `true`, or `false` on NULL args / already attached / OOM |
| `UfsrvFuture *UfsrvFutureFromValue(void *value, void (*free_value)(void *))` | An already-succeeded future. | future, or NULL on OOM |
| `UfsrvFuture *UfsrvFutureFromError(int error)` | An already-failed future. | future, or NULL on OOM |

`UfsrvFutureResult` is `{ int error; void *value; void (*free_value)(void *); }`. The
value in a result handed to a continuation or returned by `Get` is **borrowed** — the
future still owns it and frees it at refcount zero. Read it; do not free it.

A continuation must not modify the result or take ownership of its value. The completion
callback signature is `void (*)(UfsrvFutureResult *, void *context)`.

### 6.3 Operators

Each operator returns a **new** future; the input future is never consumed by `Map`,
`MapEx`, `Zip` or `ZipEx`, which the caller still owns and must release.

| Function | Callback | Value semantics | Errors |
|---|---|---|---|
| `UfsrvFuture *UfsrvFutureMap(f, mapper, ctx)` | `void *(*)(const void *, void *)` | **Borrows** the input; the mapper returns a new value, owned by the output future and released with `free`. | Propagate unchanged; a NULL mapper return fails the output with −1 |
| `UfsrvFuture *UfsrvFutureMapEx(f, mapper, ctx, free_value)` | as `Map` | as `Map`, but the mapped value is released with `free_value` instead of `free`. NULL `free_value` → the library never releases it and the caller owns it. | as `Map` |
| `UfsrvFuture *UfsrvFutureFlatMap(f, mapper, ctx)` | `UfsrvFuture *(*)(const void *, void *)` | **Moves** the input value. The mapper returns an inner future; its result becomes the output's, and it is claimed (a second move of that inner future yields `EINVAL`). | Propagate; a NULL mapper return fails the output with −1 |
| `UfsrvFuture *UfsrvFutureOnError(f, recovery, ctx)` | `UfsrvFuture *(*)(int error, void *)` | On success the value is **moved** forward unchanged; on error the recovery future's result replaces the error. | Recovered by the callback |
| `UfsrvFuture *UfsrvFutureDoOnSuccess(f, action, ctx)` | `void (*)(const void *, void *)` | Runs the side effect on success only, then **moves** the value forward. | Propagate unchanged |

**Destructor channel.** `map`/`zip` store `free` for the value the callback produces —
they can only return a plain `malloc` block. `mapEx`/`zipEx` exist so a mapper or zipper
can return a pooled, arena-backed or refcounted object: they take the destructor as a
parameter, and NULL means the caller keeps ownership. This is the only value path in the
library that does not carry its destructor on the producing call.

**Cancellation inheritance.** A single-input operator (`map`, `mapEx`, `flatMap`,
`onError`, `doOnSuccess`) whose input carries a token attaches that same token to its
output, so cancelling the token aborts the whole chain. `flatMap` additionally attaches
the token to the future its mapper returns.

### 6.4 Combinators

| Function | Purpose | Notes |
|---|---|---|
| `UfsrvFuture *UfsrvFutureAll(UfsrvFuture **futures, size_t count)` | Succeed (no value) when every input succeeds; fail with the first error as soon as one fails. | `count == 0` → immediate success. NULL array with `count > 0` → NULL. A NULL input fails the output with `EINVAL` (otherwise the counter would never reach zero and it would hang). Input values remain owned by their futures. |
| `UfsrvFuture *UfsrvFutureZip(a, b, zipper, ctx)` | On both success, combine the two borrowed values into a new one released with `free`. | Retains both inputs until the zipper has run. Either error propagates; a NULL zipper return fails the output with −1. |
| `UfsrvFuture *UfsrvFutureZipEx(a, b, zipper, ctx, free_value)` | as `Zip` with an explicit destructor for the combined value. | NULL `free_value` → caller owns the value. |
| `UfsrvFuture *UfsrvFutureAny(UfsrvFuture **futures, size_t count)` | Complete with the first input to finish, moving its result. | `count == 0` → immediate success. NULL array with `count > 0` → NULL. Losing inputs keep their own values. |

`Zip`/`ZipEx` retain both inputs, so the idiomatic `Zip(a, b, …)` followed immediately by
`UfsrvFutureRelease(a)` is safe: the zipper still reads live values.

### 6.5 Reference counting

| Function | Purpose |
|---|---|
| `void UfsrvFutureRetain(UfsrvFuture *)` | Add a reference. |
| `void UfsrvFutureRelease(UfsrvFuture *)` | Drop a reference; at zero the future and its value are freed. NULL is a no-op. |

`UfsrvPromiseCreate` hands back a future with refcount 2 — one for the promise, one for
the caller. The caller's reference is released with `UfsrvFutureRelease`. Operator and
combinator outputs start at refcount 1 and belong to the caller.

**Move once.** `flatMap`, `onError`, `doOnSuccess` and `any` *claim* the input future when
they take its value. Claiming an already-consumed future fails the operator's output with
`EINVAL`. Reads (`Get`, `Then`, `IsReady`) are unaffected and may be repeated.

---

## 7. Cancellation — `ufsrv_cancellation.h`

Cancellation is **cooperative**: a token is a shared flag that waiters observe. The token
also owns a mutex-guarded intrusive list of one-shot callbacks; `Cancel` drains it and
runs each callback on the cancelling thread, which is how a suspended waiter is woken.

| Function | Purpose | Returns |
|---|---|---|
| `UfsrvCancellationToken *UfsrvCancellationTokenCreate(void)` | Create a token (not cancelled). | token, or NULL on OOM |
| `void UfsrvCancellationTokenCancel(UfsrvCancellationToken *)` | Mark cancelled and run the registered callbacks. Idempotent; NULL is a no-op. | — |
| `bool UfsrvCancellationTokenIsCancelled(const UfsrvCancellationToken *)` | Query. NULL → `false`. | `true`/`false` |
| `void *UfsrvCancellationTokenRegister(UfsrvCancellationToken *, UfsrvCancellationCallback, void *ctx)` | Register a one-shot callback. | Unregister handle, or NULL on invalid args / OOM |
| `void *UfsrvCancellationTokenRegisterEx(UfsrvCancellationToken *, UfsrvCancellationCallback, void *ctx, UfsrvCancellationCallback release_context)` | As `Register`, plus a hook called with `ctx` when the node is finally reclaimed — on Cancel, Unregister or Destroy. | Unregister handle, or NULL |
| `bool UfsrvCancellationTokenUnregister(void *handle)` | Consume a registration handle. | `true` if it neutralised the callback; `false` if `Cancel` had already run it |
| `void UfsrvCancellationTokenDestroy(UfsrvCancellationToken *)` | Release the caller's reference to the token. NULL is a no-op. | — |

**The handle is single-use.** `Unregister` is **not idempotent**. It claims the callback
so a later `Cancel` will not run it, drops the registrant's reference and reclaims the
node — the handle is dead afterwards, and a second call with the same handle dereferences
freed memory. Call it exactly once per handle and discard it.

**Calling it is mandatory, not optional cleanup.** `Cancel` detaches a node from the
token's list before dispatching it, so a registration whose handle is never consumed is
never reclaimed — not even by `UfsrvCancellationTokenDestroy`, which walks a list that no
longer contains it. Call `Unregister` exactly once per handle whether or not `Cancel`
already ran the callback; it returns `false` in that case, and the callback has still run.

**Callback context.** `release_context`, when supplied, is invoked exactly once with
`context_ptr` when the node is reclaimed, whichever path reclaimed it. Use it to drop a
reference the callback's context owns.

### 7.1 Attached to a future

`UfsrvFutureAttachCancellation(future, token)` makes the future complete with `ECANCELED`
when the token fires, racing the promise's own completion — exactly one of the two wins,
so the future completes once. The attachment:

- retains the future for the lifetime of the cancel callback, so a cancel that fires after
  the future was otherwise released is still safe;
- holds its own reference on the token, so destroying the caller's token reference
  immediately afterwards is safe — the token lives until both drop it;
- is released when the future completes, so a completed future is not pinned by its token;
- is inherited by single-input operators built on the future (§6.3).

---

## 8. Version — `version.h`

| Function | Returns |
|---|---|
| `const char *UfsrvRxLibVersion(void)` | Full dotted string, e.g. `"0.1.23"`. |
| `const char *UfsrvRxLibVersionMajor(void)` | `"0"` |
| `const char *UfsrvRxLibVersionMinor(void)` | `"1"` |
| `const char *UfsrvRxLibVersionPatch(void)` | `"23"` |

All four return pointers to static storage — never free them. `UFSRVRXLIB_MAJOR` /
`_MINOR` / `_PATCH` are compile-time macros; `UFSRVRXLIB_INTERNAL` is the build counter
and is not part of the released version.

---

## 9. Execution model

**Where a continuation runs.** A callback registered with `UfsrvFutureThen` (and every
operator or combinator built on it) runs on whichever thread fulfils the promise — or
immediately on the registering thread if the future is already ready. It is never
dispatched to a scheduler by the library. If the callback needs a particular thread, it
must arrange that itself.

**`UfsrvFutureGet` dispatches on context:**

| Call site | Behaviour |
|---|---|
| Future already ready | Returns the result immediately; no wait. |
| Inside a scheduler-driven coroutine | Suspends the coroutine (`UfsrvCoroutineYield`) and resumes it on its worker when the future completes or its token is cancelled. |
| A plain POSIX thread | Blocks on an eventfd until the future completes. |
| A scheduler worker thread, but not inside a coroutine | Refused: asserts in debug builds and returns `EDEADLK` in release builds. Blocking there would park the worker and deadlock the future that worker is meant to complete. |

`UfsrvFutureGetWithCancellation` follows the same table; when a token is supplied, a
cancelled token short-circuits to `ECANCELED` without waiting, and a cancel during the
wait wakes the waiter (coroutine resume, or an eventfd write for a blocking thread).

**Cancellation is observed, never pre-empted.** Nothing is interrupted asynchronously: a
waiter notices the token because the token's callback resumes it, and any other
long-running work notices only if it polls `UfsrvCancellationTokenIsCancelled` at its own
yield points.

**Coroutine affinity.** Coroutines are thread-bound (§5). Jobs on a pool run on different
workers concurrently; a callback that touches shared state must synchronise it.

---

## 10. Ownership and lifetime

| Object | Created by | Released by | Notes |
|---|---|---|---|
| `UfsrvPromise` | `UfsrvPromiseCreate` | `UfsrvPromiseDestroy` | Always, fulfilled or not. Destroying an unfulfilled promise completes its future with `EPIPE`. |
| `UfsrvFuture` | `UfsrvPromiseCreate` (refcount 2), operators/combinators (refcount 1), `FromValue`/`FromError` (refcount 1) | `UfsrvFutureRelease` | `Retain` adds a reference. |
| Result value | the producer, via `SetValue`/`SetResult`/`FromValue` | the future, via the result's `free_value` at refcount zero | A value produced by `Map`/`Zip` is released with `free`; by `MapEx`/`ZipEx` with the destructor you supplied. |
| Mapped/zipped value | the mapper or zipper callback | the output future, with `free` (or your `free_value`) | Returning NULL fails the output with −1. |
| `UfsrvCancellationToken` | `UfsrvCancellationTokenCreate` | `UfsrvCancellationTokenDestroy` | An attached future holds its own reference. |
| Registration handle | `Register`/`RegisterEx` | `UfsrvCancellationTokenUnregister`, exactly once | Single-use; never reclaimed otherwise. |
| `UfsrvScheduler` / `…Pool` | `…Create` | `…Destroy` | |
| `UfsrvCoroutine` | `Create` / `Spawn` | itself, on exit; or `UfsrvCoroutineThreadCleanup` if never resumed | The handle is dead after either. |
| `BufferDescriptor` (from `DescribeScheduler`) | caller, or the function when `provided` is NULL | `BufferDescriptorRelease` | |
| `CollectionDescriptor` (from `ListSchedulerWorkers`) | the function | `free()` — one call | |

Two rules cover most mistakes: **the library frees only what it was given a destructor
for**, and **a borrowed value is never yours to free**.

---

## 11. Error codes

| Code | Meaning |
|---|---|
| `0` | Success. |
| `ECANCELED` | The token was cancelled — returned by `GetWithCancellation`, or set on a future by `AttachCancellation`. |
| `EPIPE` | Broken promise: `UfsrvPromiseDestroy` was called on a promise that was never fulfilled. |
| `EINVAL` | A move operator found its input already consumed; or `All` was handed a NULL input. |
| `EDEADLK` | A blocking `Get` was attempted on a scheduler worker thread (release builds; debug builds assert). |
| `-1` | Allocation failure, a NULL argument, or a mapper/zipper that returned NULL. |

Operators propagate the first error downstream unchanged; `Map`/`FlatMap`/`Zip` never run
their callbacks on an error input. `OnError` is the recovery point.

---

## 12. Recipes

**Fluent chain — transform, chain, recover, observe.**

```c
UfsrvFuture *f = fetch_user(id);                        /* some future */
UfsrvFuture *orders  = UfsrvFutureFlatMap(f, fetch_orders, NULL);  /* async A → Future<B> */
UfsrvFuture *summary = UfsrvFutureMap(orders, summarize, NULL);    /* sync  B → C */
UfsrvFuture *safe    = UfsrvFutureOnError(summary, fallback, NULL);/* recover */
UfsrvFutureThen(safe, on_done, NULL);                   /* terminal */

UfsrvFutureRelease(safe); UfsrvFutureRelease(summary);
UfsrvFutureRelease(orders); UfsrvFutureRelease(f);
```

Keep every intermediate alive until the chain has drained — an operator borrows its
input, so releasing too early can hand the next stage a freed value.

**Explicit destructor for a non-malloc value.**

```c
UfsrvFuture *mapped = UfsrvFutureMapEx(future, mapper_returning_pooled_object, NULL, pool_release);
UfsrvFuture *zipped = UfsrvFutureZipEx(a, b, zipper_returning_refcounted_object, NULL, object_unref);
```

**Cancellable pipeline.**

```c
UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
UfsrvFutureAttachCancellation(source, token);       /* whole chain inherits it */
UfsrvFuture *out = UfsrvFutureFlatMap(source, stage_two, NULL);
UfsrvFutureThen(out, on_done, NULL);

/* another thread */  UfsrvCancellationTokenCancel(token);   /* out completes ECANCELED */
UfsrvCancellationTokenDestroy(token);               /* drop your own reference */
```

**Awaited result inside a coroutine.**

```c
static void sWorker(void) {
    UfsrvFutureResult r = UfsrvFutureGetWithCancellation(future, token);
    if (r.error == 0) { consume(r.value); }         /* borrowed */
    UfsrvCoroutineExit();
}
```

**Raw registration with a context-release hook.**

```c
void *h = UfsrvCancellationTokenRegisterEx(token, on_cancel, ctx, release_ctx);
/* … */
UfsrvCancellationTokenUnregister(h);   /* once; h is dead afterwards */
h = NULL;                              /* do not reuse or double-unregister */
```

---

## 13. Examples

Runnable programs under `examples/`, built when the examples target is enabled:

| Program | Demonstrates |
|---|---|
| `async_image_download.c` | The full stack: a coroutine on a worker thread does the work and fulfils a future while the main thread stays free to render progress. |
| `slots.c` | Coroutine execution models: `cooperative` (one worker round-robins N coroutines, M:N) and `--parallel` (one worker per coroutine, 1:1). |
| `blocking_get.c` | `UfsrvFutureGet` on a plain (non-coroutine) thread taking the eventfd blocking path. |
| `cancellation.c` | `AttachCancellation` + `Cancel`: the future completes `ECANCELED` instead of the result; `--late` lets the work finish first. |
| `cancellation_chain.c` | Cancellation propagating through a `flatMap` chain, including the mapper's inner future. |
| `blocking_cancellable.c` | A plain thread blocking in `GetWithCancellation` with a timeout token: `ECANCELED` on timeout, value if the work finishes first. |

---

## 14. Build and link

```sh
cmake -S . -B build
cmake --build build
```

| | |
|---|---|
| CMake target | `ufsrvrxlib::ufsrvrxlib` (static by default; shared when enabled) |
| Link line | `-lufsrvrxlib` |
| Dependency | system-installed `uflib` (the layer beneath); libaco is built alongside as `ufsrvrxlib_aco` |
| Install | `cmake --install build --prefix /usr/local` — headers, `libufsrvrxlib`, pkg-config file, and the `ufsrvrxlib(7)` man page |

---

## 15. Contract violations

These are caller errors, not library bugs. Each one is undefined behaviour.

| Violation | Consequence |
|---|---|
| Calling `UfsrvCancellationTokenUnregister` twice with the same handle | Use-after-free. The handle is consumed by the first call. |
| Never calling `UfsrvCancellationTokenUnregister` for a registration | The node is leaked permanently — `Cancel` detaches it, so `Destroy` cannot reach it. |
| Freeing a value the library still owns (one handed to `SetValue`, `FromValue`, `MapEx`, `ZipEx`, …) | Double free. |
| Freeing a borrowed value returned by `Get`/`Then` | Double free. |
| Using a coroutine handle after the coroutine exited, or after `UfsrvCoroutineThreadCleanup` reclaimed it | Use-after-free. |
| Blocking in `Get` on a scheduler worker thread | Asserts in debug; `EDEADLK` in release. |
| Moving the same future twice (`flatMap`/`onError`/`doOnSuccess`/`any`) | The second move fails its output with `EINVAL`. |
| Submitting to a scheduler that was never started or has stopped | `false`; the job never runs. |
| Resuming a coroutine from a thread other than its owner | Undefined. Use `UfsrvCoroutineSubmitResume`. |

## Version Management

| Version | Date Modified | Modified By | Description                                                                                  |
|---------|---------------|-------------|----------------------------------------------------------------------------------------------|
| 0.1.0   | 2026-10-03    | devops      | Initial API guide: complete public surface (62 functions), ownership, error and execution models, recipes, examples |
