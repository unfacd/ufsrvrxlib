# ufsrvrxlib

**Reactive extensions for C — futures, promises, coroutines and a scheduler.**

`ufsrvrxlib` is the asynchronous-programming library for the ufsrv messaging-server
platform. It brings the composition style of RxJava, Guava's `ListenableFuture` and
Java's `CompletableFuture` to C17: fluent chains of `map` / `flatMap` / `onError`,
sequential-looking code that waits without blocking a thread, and cooperative
cancellation — over a scheduler you place yourself.

## Design goals

| Goal | Meaning |
|---|---|
| Composable async in C | `map`, `flatMap`, `onError`, `all`, `zip` behind opaque handles and a plain C ABI. |
| No hidden global scheduler | You create the threads and decide which worker runs what; nothing runs behind your back. |
| M:N concurrency | Many coroutines on few threads — a suspended coroutine costs a stack, not an OS thread. |
| Strict layering | Each layer depends only on the one beneath it, and owns exactly one responsibility. |

## The layer cake

```
L1   your code           fluent chains, or sequential await-style reads
L2   futures / promises  composable, non-blocking result delivery
L3   coroutines          stackful suspend and resume
L4   scheduler           worker threads over a lock-free MPSC queue and an eventfd
```

Each layer depends only on the one directly beneath it. The scheduler sees opaque job
callbacks only — never result semantics, never a coroutine stack.

| Layer | Module | Responsibility |
|---|---|---|
| L4 | `ufsrv_scheduler` | One OS thread, one lock-free multi-producer queue and one `eventfd` per scheduler. Pools spread work round-robin, or pin a job to a named worker. |
| L3 | `ufsrv_coroutine` | Stackful coroutines bound to the thread that creates them: spawn onto a worker, suspend at a yield, resume through the scheduler. |
| L2 | `ufsrv_future` | The value itself: a one-shot promise feeding a future, continuations, operators, combinators, and reads that suspend or block. |
| — | `ufsrv_cancellation` | Cooperative cancellation: a token cancels the work and wakes whatever is waiting on it. |
| — | `version` | Runtime version accessors. |

## A first program

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
    struct Job *job = UfsrvCoroutineGetArg();

    int *value = malloc(sizeof(*value));
    *value = 42;
    UfsrvPromiseSetValue(job->promise, value, free);  /* completes the future */
    UfsrvPromiseDestroy(job->promise);                /* Set* never frees it */
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

## Semantics you must honour

| Rule | Why |
|---|---|
| A promise is one-shot, and `Set*` never frees it | The first fulfilment wins; always call `UfsrvPromiseDestroy`, fulfilled or not. Destroying an unfulfilled promise completes its future with `EPIPE`, so an abandoned promise reports itself instead of hanging. |
| `Get` and `Then` hand back a *borrowed* value | Read it; never free it. The future frees it once, at reference-count zero. |
| `map` borrows, while `flatMap` / `onError` / `doOnSuccess` move | A mover takes ownership of the input value and the input future no longer holds it; a second move on the same future fails with `EINVAL`. Reads are unaffected and may be repeated. |
| A mapper's or zipper's return value is the one the library frees for you | With `free()`, so `map` / `zip` callbacks may only return a `malloc` block. Use `MapEx` / `ZipEx` to supply the destructor for a pooled, arena-backed or reference-counted value. |
| Cancellation is cooperative | It is observed at yield points and at a waiting `Get`; it never pre-empts running code. |
| Do not block a worker | `UfsrvFutureGet` on a scheduler worker thread outside a coroutine refuses with `EDEADLK`, because blocking there would park the worker every other coroutine shares. |
| A coroutine belongs to the thread that created it | Resume it only from that thread. To get back to it, post the resume through its scheduler with `UfsrvCoroutineSubmitResume`; a coroutine never migrates. |

Error codes are `0` for success, `ECANCELED`, `EPIPE`, `EINVAL`, `EDEADLK`, and `-1`
for allocation failure or a NULL argument. Operators propagate the first error
downstream.

## Current constraints

- Work-stealing load balancing between workers (the per-worker queue stays the
  cross-thread injection channel).
- Multi-core parallelism for CPU-bound work — coroutines switch only where they yield.
- A central dispatcher thread, deliberately: the submitting thread picks the target.

## Install

Debian and Ubuntu hosts (amd64) can install the packaged library from the signed
apt repository:

```sh
curl -fsSL https://unfacd.github.io/ufsrvrxlib/install.sh | sudo bash
```

That establishes the signing key, points apt at `https://unfacd.github.io/ufsrvrxlib`
and installs `ufsrvrxlib-dev` — headers, the static libraries, the pkg-config file
and the man page. The package depends on `uflib-dev`, served from
<https://unfacd.github.io/uflib>; the installer configures that repository too, and
apt resolves the dependency itself. The landing page at that first URL carries the
manual steps and the key fingerprints.

## Build and link

| | |
|---|---|
| Language and runtime | C17, POSIX threads and `eventfd` (Linux) |
| Dependency | `uflib`, the layer beneath — taken from an in-source `packages/uflib` checkout, the system install, or a fetch when neither is present |
| Coroutine runtime | libaco, compiled into the library — not a system package to install |
| Build system | CMake 3.20 or later |

```sh
cmake -S . -B build
cmake --build build
cmake --install build --prefix /usr/local
```

The coroutine runtime is vendored under `packages/`, which is excluded from version
control; restore it before configuring a fresh checkout. A `packages/uflib` checkout, if
one is present, is used in preference to an installed `uflib`. The fetch source is
`UFSRVRXLIB_UFLIB_SOURCE` (a git URL or a local path) and the branch or tag is
`UFSRVRXLIB_DEP_GIT_TAG`; both are cache variables, settable with `-D`.

Consumers link `-lufsrvrxlib`. The install also provides a pkg-config file
(`ufsrvrxlib`) and a CMake package (`ufsrvrxlib::ufsrvrxlib`). The runnable programs
under `examples/` cover the whole stack and build with the library. The version is
available at runtime from `UfsrvRxLibVersion()` and at compile time in
`<ufsrvrxlib/version.h>`.

## Documentation

| Document | Contents |
|---|---|
| `UFSRVRXLIB_API_GUIDE.md` | The complete public surface: every function's semantics, ownership, threading and failure contract, with recipes and a list of caller errors that are undefined behaviour |
| `ufsrvrxlib(7)` | The man page: module tour, lifetime rules, cancellation model, worked example |
| `examples/` | Runnable programs: a full-stack async download, coroutine execution models, blocking reads, cancellation, and cancellation through a chain |

The authoritative declarations are the installed headers under `<ufsrvrxlib/>` — one
directory per module, plus `version.h` and `ufsrvrxlib_defs.h` at the top level.

## Licence

Copyright © 2015–2026 unfacd works. Distributed under the GNU Affero General Public
License, version 3 or later.
