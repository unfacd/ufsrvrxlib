/**
 * @file ufsrv_future_tests.cpp
 * @brief Adversarial gtest suite for the ufsrv_future module.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <thread>

#include <ufsrvrxlib/ufsrv_future/ufsrv_future.h>
#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>
#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>
#include <ufsrvrxlib/ufsrv_cancellation/ufsrv_cancellation.h>

namespace {

/*! Captures a delivered result and counts completions. */
struct Completion {
    std::atomic<int> count{0};
    UfsrvFutureResult result{};
};

void OnDone(UfsrvFutureResult *result_ptr, void *context_ptr) {
    auto *completion = static_cast<Completion *>(context_ptr);
    completion->result = *result_ptr;
    completion->count.fetch_add(1, std::memory_order_relaxed);
}

/*! Doubles a heap-allocated int (freed via `free`). */
void *DoubleMapper(const void *value_ptr, void *context_ptr) {
    (void)context_ptr;
    int *out = static_cast<int *>(malloc(sizeof(int)));
    *out = *static_cast<const int *>(value_ptr) * 2;
    return out;
}

/*! Async mapper: returns a ready future of the doubled value. */
UfsrvFuture *AsyncDouble(const void *value_ptr, void *context_ptr) {
    (void)context_ptr;
    int *out = static_cast<int *>(malloc(sizeof(int)));
    *out = *static_cast<const int *>(value_ptr) * 2;
    return UfsrvFutureFromValue(out, free);
}

/*! Recovery: returns a ready future of 100 + error. */
UfsrvFuture *Recover(int error, void *context_ptr) {
    (void)context_ptr;
    int *out = static_cast<int *>(malloc(sizeof(int)));
    *out = 100 + error;
    return UfsrvFutureFromValue(out, free);
}

/*! FlatMap cancellation: the mapper returns a pending inner future. */
struct FlatMapCancelContext {
    UfsrvFuture *inner;
};

/*! Mapper: hand the flatMap a reference to a specific (pending) inner future. */
UfsrvFuture *ReturnInner(const void *value_ptr, void *context_ptr) {
    (void)value_ptr;
    auto *ctx = static_cast<FlatMapCancelContext *>(context_ptr);
    UfsrvFutureRetain(ctx->inner);   /* give the flatMap a reference it will release */
    return ctx->inner;
}

static std::atomic<int> g_action_count{0};

void RecordAction(const void *value_ptr, void *context_ptr) {
    (void)value_ptr;
    (void)context_ptr;
    g_action_count.fetch_add(1, std::memory_order_relaxed);
}

/*! Zipper: sum two ints (freed via `free`). */
void *SumZipper(const void *value_a_ptr, const void *value_b_ptr, void *context_ptr) {
    (void)context_ptr;
    int *out = static_cast<int *>(malloc(sizeof(int)));
    *out = *static_cast<const int *>(value_a_ptr) + *static_cast<const int *>(value_b_ptr);
    return out;
}

/*! Job that completes a promise with a value, run on another worker. */
struct CompleteJob {
    UfsrvPromise *promise;
    int value;
};

void CompleteJobFn(void *context_ptr) {
    auto *job = static_cast<CompleteJob *>(context_ptr);
    int *v = static_cast<int *>(malloc(sizeof(int)));
    *v = job->value;
    UfsrvPromiseSetValue(job->promise, v, free);
    free(job);
}

/*! Context for the get() coroutine. */
struct GetContext {
    UfsrvScheduler *scheduler;
    UfsrvScheduler *other;
    std::atomic<int> result{-1};
    std::atomic<int> done{0};
};

/*! Entry: complete a promise on another worker, then await it via UfsrvFutureGet. */
void GetEntry(void) {
    auto *ctx = static_cast<GetContext *>(UfsrvCoroutineGetArg());

    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);

    auto *job = static_cast<CompleteJob *>(malloc(sizeof(CompleteJob)));
    job->promise = promise;
    job->value = 42;
    UfsrvSchedulerSubmit(ctx->other, CompleteJobFn, job);

    UfsrvFutureResult result = UfsrvFutureGet(future);
    ctx->result.store(result.error == 0 ? *static_cast<int *>(result.value) : -1,
                      std::memory_order_relaxed);
    ctx->done.store(1, std::memory_order_relaxed);
    UfsrvFutureRelease(future);
    UfsrvCoroutineExit();
}

/*! Re-entrant continuation: registers itself once more. */
struct ReentrantCtx {
    UfsrvFuture *future;
    std::atomic<int> count{0};
};

void ReentrantOnDone(UfsrvFutureResult *result_ptr, void *context_ptr) {
    (void)result_ptr;
    auto *ctx = static_cast<ReentrantCtx *>(context_ptr);
    if (ctx->count.fetch_add(1, std::memory_order_relaxed) == 0) {
        UfsrvFutureThen(ctx->future, ReentrantOnDone, ctx);
    }
}

/*! Context for the get()-with-cancellation coroutine. */
struct CancelGetContext {
    UfsrvScheduler       *other;   /*!< Completes the promise (NULL → never completed). */
    UfsrvCancellationToken *token;
    std::atomic<int>      reached{0};
    std::atomic<int>      done{0};
    std::atomic<int>      error{0};
    std::atomic<int>      value{-1};
};

/*! Entry: await a future with cancellation; optionally complete it on another worker. */
void CancelGetEntry(void) {
    auto *ctx = static_cast<CancelGetContext *>(UfsrvCoroutineGetArg());

    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);

    if (ctx->other != nullptr) {
        auto *job = static_cast<CompleteJob *>(malloc(sizeof(CompleteJob)));
        job->promise = promise;
        job->value = 42;
        UfsrvSchedulerSubmit(ctx->other, CompleteJobFn, job);
    }

    ctx->reached.store(1, std::memory_order_release);

    UfsrvFutureResult result = UfsrvFutureGetWithCancellation(future, ctx->token);
    ctx->error.store(result.error, std::memory_order_relaxed);
    if (result.error == 0 && result.value != nullptr) {
        ctx->value.store(*static_cast<int *>(result.value), std::memory_order_relaxed);
    }

    ctx->done.store(1, std::memory_order_release);

    if (ctx->other == nullptr) {
        UfsrvPromiseDestroy(promise);   /* never completed — abandon its reference */
    }
    UfsrvFutureRelease(future);
    UfsrvCoroutineExit();
}

/*! Context for the cancel-vs-complete race coroutine (future/promise owned by main). */
struct RaceContext {
    UfsrvFuture           *future;
    UfsrvCancellationToken *token;
    std::atomic<int>       reached{0};
    std::atomic<int>       done{0};
    std::atomic<int>       error{0};
    std::atomic<int>       value{-1};
};

/*! Entry: await a future that the main thread races cancel against completion. */
void RaceGetEntry(void) {
    auto *ctx = static_cast<RaceContext *>(UfsrvCoroutineGetArg());

    ctx->reached.store(1, std::memory_order_release);

    UfsrvFutureResult result = UfsrvFutureGetWithCancellation(ctx->future, ctx->token);
    ctx->error.store(result.error, std::memory_order_relaxed);
    if (result.error == 0 && result.value != nullptr) {
        ctx->value.store(*static_cast<int *>(result.value), std::memory_order_relaxed);
    }

    ctx->done.store(1, std::memory_order_release);
    UfsrvCoroutineExit();
}

}  // namespace

/* ── Promise / completion ─────────────────────────────────────────────────── */

TEST(UfsrvFutureTest, SetValueThenRuns) {
    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    ASSERT_NE(promise, nullptr);
    ASSERT_NE(future, nullptr);
    EXPECT_FALSE(UfsrvFutureIsReady(future));

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(future, OnDone, &completion));

    int *value = static_cast<int *>(malloc(sizeof(int)));
    *value = 42;
    UfsrvPromiseSetValue(promise, value, free);

    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 0);
    EXPECT_EQ(*static_cast<int *>(completion.result.value), 42);

    UfsrvFutureRelease(future);
}

TEST(UfsrvFutureTest, ThenAfterCompletionRunsInline) {
    UfsrvFuture *future = UfsrvFutureFromValue(nullptr, nullptr);
    ASSERT_NE(future, nullptr);
    EXPECT_TRUE(UfsrvFutureIsReady(future));

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(future, OnDone, &completion));
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 0);

    UfsrvFutureRelease(future);
}

TEST(UfsrvFutureTest, SetErrorPropagates) {
    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    ASSERT_NE(promise, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(future, OnDone, &completion));

    UfsrvPromiseSetError(promise, 7);
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 7);
    EXPECT_EQ(completion.result.value, nullptr);

    UfsrvFutureRelease(future);
}

TEST(UfsrvFutureTest, FromError) {
    UfsrvFuture *future = UfsrvFutureFromError(9);
    ASSERT_NE(future, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(future, OnDone, &completion));
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 9);

    UfsrvFutureRelease(future);
}

TEST(UfsrvFutureTest, ReentrantThen) {
    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    ASSERT_NE(promise, nullptr);

    ReentrantCtx ctx{future, 0};
    ASSERT_TRUE(UfsrvFutureThen(future, ReentrantOnDone, &ctx));

    UfsrvPromiseSetValue(promise, nullptr, nullptr);
    EXPECT_EQ(ctx.count.load(), 2);

    UfsrvFutureRelease(future);
}

/* ── Map ──────────────────────────────────────────────────────────────────── */

TEST(UfsrvFutureTest, MapTransforms) {
    UfsrvFuture *src = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&src);
    ASSERT_NE(promise, nullptr);

    UfsrvFuture *mapped = UfsrvFutureMap(src, DoubleMapper, nullptr);
    ASSERT_NE(mapped, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(mapped, OnDone, &completion));

    int *value = static_cast<int *>(malloc(sizeof(int)));
    *value = 21;
    UfsrvPromiseSetValue(promise, value, free);

    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 0);
    EXPECT_EQ(*static_cast<int *>(completion.result.value), 42);

    UfsrvFutureRelease(mapped);
    UfsrvFutureRelease(src);
}

TEST(UfsrvFutureTest, MapPropagatesError) {
    UfsrvFuture *src = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&src);
    ASSERT_NE(promise, nullptr);

    UfsrvFuture *mapped = UfsrvFutureMap(src, DoubleMapper, nullptr);
    ASSERT_NE(mapped, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(mapped, OnDone, &completion));

    UfsrvPromiseSetError(promise, 5);
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 5);
    EXPECT_EQ(completion.result.value, nullptr);

    UfsrvFutureRelease(mapped);
    UfsrvFutureRelease(src);
}

/* ── FlatMap ──────────────────────────────────────────────────────────────── */

TEST(UfsrvFutureTest, FlatMapChainsAsync) {
    UfsrvFuture *src = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&src);
    ASSERT_NE(promise, nullptr);

    UfsrvFuture *chained = UfsrvFutureFlatMap(src, AsyncDouble, nullptr);
    ASSERT_NE(chained, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(chained, OnDone, &completion));

    int *value = static_cast<int *>(malloc(sizeof(int)));
    *value = 21;
    UfsrvPromiseSetValue(promise, value, free);

    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 0);
    EXPECT_EQ(*static_cast<int *>(completion.result.value), 42);

    UfsrvFutureRelease(chained);
    UfsrvFutureRelease(src);
}

TEST(UfsrvFutureTest, FlatMapPropagatesError) {
    UfsrvFuture *src = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&src);
    ASSERT_NE(promise, nullptr);

    UfsrvFuture *chained = UfsrvFutureFlatMap(src, AsyncDouble, nullptr);
    ASSERT_NE(chained, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(chained, OnDone, &completion));

    UfsrvPromiseSetError(promise, 5);
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 5);

    UfsrvFutureRelease(chained);
    UfsrvFutureRelease(src);
}

/* ── OnError ──────────────────────────────────────────────────────────────── */

TEST(UfsrvFutureTest, OnErrorRecovers) {
    UfsrvFuture *src = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&src);
    ASSERT_NE(promise, nullptr);

    UfsrvFuture *recovered = UfsrvFutureOnError(src, Recover, nullptr);
    ASSERT_NE(recovered, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(recovered, OnDone, &completion));

    UfsrvPromiseSetError(promise, 5);
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 0);
    EXPECT_EQ(*static_cast<int *>(completion.result.value), 105);

    UfsrvFutureRelease(recovered);
    UfsrvFutureRelease(src);
}

TEST(UfsrvFutureTest, OnErrorForwardsSuccess) {
    UfsrvFuture *src = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&src);
    ASSERT_NE(promise, nullptr);

    UfsrvFuture *forwarded = UfsrvFutureOnError(src, Recover, nullptr);
    ASSERT_NE(forwarded, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(forwarded, OnDone, &completion));

    int *value = static_cast<int *>(malloc(sizeof(int)));
    *value = 42;
    UfsrvPromiseSetValue(promise, value, free);

    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 0);
    EXPECT_EQ(*static_cast<int *>(completion.result.value), 42);  // forwarded, not recovered

    UfsrvFutureRelease(forwarded);
    UfsrvFutureRelease(src);
}

/* ── DoOnSuccess ──────────────────────────────────────────────────────────── */

TEST(UfsrvFutureTest, DoOnSuccessSideEffectAndForward) {
    UfsrvFuture *src = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&src);
    ASSERT_NE(promise, nullptr);

    g_action_count.store(0);
    UfsrvFuture *forwarded = UfsrvFutureDoOnSuccess(src, RecordAction, nullptr);
    ASSERT_NE(forwarded, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(forwarded, OnDone, &completion));

    int *value = static_cast<int *>(malloc(sizeof(int)));
    *value = 7;
    UfsrvPromiseSetValue(promise, value, free);

    EXPECT_EQ(g_action_count.load(), 1);
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 0);
    EXPECT_EQ(*static_cast<int *>(completion.result.value), 7);

    UfsrvFutureRelease(forwarded);
    UfsrvFutureRelease(src);
}

/* ── Combinators ──────────────────────────────────────────────────────────── */

TEST(UfsrvFutureTest, AllSuccess) {
    UfsrvFuture *f1 = nullptr, *f2 = nullptr;
    UfsrvPromise *p1 = UfsrvPromiseCreate(&f1);
    UfsrvPromise *p2 = UfsrvPromiseCreate(&f2);
    UfsrvFuture *futures[] = {f1, f2};
    UfsrvFuture *all = UfsrvFutureAll(futures, 2);
    ASSERT_NE(all, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(all, OnDone, &completion));

    UfsrvPromiseSetValue(p1, nullptr, nullptr);
    EXPECT_EQ(completion.count.load(), 0);  // not done yet
    UfsrvPromiseSetValue(p2, nullptr, nullptr);
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 0);

    UfsrvFutureRelease(all);
    UfsrvFutureRelease(f1);
    UfsrvFutureRelease(f2);
}

TEST(UfsrvFutureTest, AllFailFast) {
    UfsrvFuture *f1 = nullptr, *f2 = nullptr;
    UfsrvPromise *p1 = UfsrvPromiseCreate(&f1);
    UfsrvPromise *p2 = UfsrvPromiseCreate(&f2);
    UfsrvFuture *futures[] = {f1, f2};
    UfsrvFuture *all = UfsrvFutureAll(futures, 2);
    ASSERT_NE(all, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(all, OnDone, &completion));

    UfsrvPromiseSetError(p1, 5);
    EXPECT_EQ(completion.count.load(), 1);  // fail-fast
    EXPECT_EQ(completion.result.error, 5);
    UfsrvPromiseSetValue(p2, nullptr, nullptr);  // completes later; no-op

    UfsrvFutureRelease(all);
    UfsrvFutureRelease(f1);
    UfsrvFutureRelease(f2);
}

TEST(UfsrvFutureTest, AllEmpty) {
    UfsrvFuture *all = UfsrvFutureAll(nullptr, 0);
    ASSERT_NE(all, nullptr);
    EXPECT_TRUE(UfsrvFutureIsReady(all));
    UfsrvFutureRelease(all);
}

TEST(UfsrvFutureTest, ZipSuccess) {
    UfsrvFuture *f1 = nullptr, *f2 = nullptr;
    UfsrvPromise *p1 = UfsrvPromiseCreate(&f1);
    UfsrvPromise *p2 = UfsrvPromiseCreate(&f2);
    UfsrvFuture *zipped = UfsrvFutureZip(f1, f2, SumZipper, nullptr);
    ASSERT_NE(zipped, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(zipped, OnDone, &completion));

    int *a = static_cast<int *>(malloc(sizeof(int)));
    *a = 20;
    int *b = static_cast<int *>(malloc(sizeof(int)));
    *b = 22;
    UfsrvPromiseSetValue(p1, a, free);
    EXPECT_EQ(completion.count.load(), 0);
    UfsrvPromiseSetValue(p2, b, free);
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 0);
    EXPECT_EQ(*static_cast<int *>(completion.result.value), 42);

    UfsrvFutureRelease(zipped);
    UfsrvFutureRelease(f1);
    UfsrvFutureRelease(f2);
}

TEST(UfsrvFutureTest, ZipErrorPropagates) {
    UfsrvFuture *f1 = nullptr, *f2 = nullptr;
    UfsrvPromise *p1 = UfsrvPromiseCreate(&f1);
    UfsrvPromise *p2 = UfsrvPromiseCreate(&f2);
    UfsrvFuture *zipped = UfsrvFutureZip(f1, f2, SumZipper, nullptr);
    ASSERT_NE(zipped, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(zipped, OnDone, &completion));

    UfsrvPromiseSetError(p1, 9);
    UfsrvPromiseSetValue(p2, nullptr, nullptr);
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 9);

    UfsrvFutureRelease(zipped);
    UfsrvFutureRelease(f1);
    UfsrvFutureRelease(f2);
}

TEST(UfsrvFutureTest, AnyFirstWins) {
    UfsrvFuture *f1 = nullptr, *f2 = nullptr;
    UfsrvPromise *p1 = UfsrvPromiseCreate(&f1);
    UfsrvPromise *p2 = UfsrvPromiseCreate(&f2);
    UfsrvFuture *futures[] = {f1, f2};
    UfsrvFuture *any = UfsrvFutureAny(futures, 2);
    ASSERT_NE(any, nullptr);

    Completion completion;
    ASSERT_TRUE(UfsrvFutureThen(any, OnDone, &completion));

    int *a = static_cast<int *>(malloc(sizeof(int)));
    *a = 7;
    UfsrvPromiseSetValue(p1, a, free);
    EXPECT_EQ(completion.count.load(), 1);
    EXPECT_EQ(completion.result.error, 0);
    EXPECT_EQ(*static_cast<int *>(completion.result.value), 7);

    int *b = static_cast<int *>(malloc(sizeof(int)));
    *b = 8;
    UfsrvPromiseSetValue(p2, b, free);  // loser; value stays owned by f2

    UfsrvFutureRelease(any);
    UfsrvFutureRelease(f1);
    UfsrvFutureRelease(f2);
}

/* ── Cooperative get() ───────────────────────────────────────────────────── */

TEST(UfsrvFutureTest, GetYieldsAndResumes) {
    UfsrvScheduler *s1 = UfsrvSchedulerCreate("g1");
    UfsrvScheduler *s2 = UfsrvSchedulerCreate("g2");
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    ASSERT_EQ(UfsrvSchedulerStart(s1), 0);
    ASSERT_EQ(UfsrvSchedulerStart(s2), 0);

    GetContext ctx{s1, s2, -1, 0};
    UfsrvCoroutineSpawn(s1, GetEntry, &ctx);

    for (int i = 0; i < 10000 && ctx.done.load() < 1; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    UfsrvSchedulerStop(s1);
    UfsrvSchedulerJoin(s1);
    UfsrvSchedulerDestroy(s1);
    UfsrvSchedulerStop(s2);
    UfsrvSchedulerJoin(s2);
    UfsrvSchedulerDestroy(s2);

    EXPECT_EQ(ctx.done.load(), 1);
    EXPECT_EQ(ctx.result.load(), 42);
}

/* ── Blocking get() (plain thread) ────────────────────────────────────────── */

TEST(UfsrvFutureTest, BlockingGetOnPlainThread) {
    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    ASSERT_NE(promise, nullptr);

    std::thread completer([promise]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        int *v = static_cast<int *>(malloc(sizeof(int)));
        *v = 99;
        UfsrvPromiseSetValue(promise, v, free);
    });

    /* Block on the plain (main) thread until the completer finishes. */
    UfsrvFutureResult r = UfsrvFutureGet(future);
    completer.join();

    EXPECT_EQ(r.error, 0);
    ASSERT_NE(r.value, nullptr);
    EXPECT_EQ(*static_cast<int *>(r.value), 99);

    UfsrvFutureRelease(future);
}

TEST(UfsrvFutureTest, BlockingGetStress) {
    for (int it = 0; it < 200; it++) {
        UfsrvFuture *future = nullptr;
        UfsrvPromise *promise = UfsrvPromiseCreate(&future);
        ASSERT_NE(promise, nullptr);

        std::thread completer([promise, it]() {
            int *v = static_cast<int *>(malloc(sizeof(int)));
            *v = it;
            UfsrvPromiseSetValue(promise, v, free);
        });

        UfsrvFutureResult r = UfsrvFutureGet(future);   /* race: complete vs block */
        completer.join();

        EXPECT_EQ(r.error, 0);
        ASSERT_NE(r.value, nullptr);
        EXPECT_EQ(*static_cast<int *>(r.value), it);

        UfsrvFutureRelease(future);
    }
}

TEST(UfsrvFutureTest, BlockingGetWithCancellationCancelWakesThread) {
    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    ASSERT_NE(promise, nullptr);

    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    ASSERT_NE(token, nullptr);

    std::thread canceller([token]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        UfsrvCancellationTokenCancel(token);
    });

    /* Block on the plain (main) thread; cancel wakes it → ECANCELED. */
    UfsrvFutureResult r = UfsrvFutureGetWithCancellation(future, token);
    canceller.join();

    EXPECT_EQ(r.error, ECANCELED);

    UfsrvPromiseDestroy(promise);
    UfsrvFutureRelease(future);
    UfsrvCancellationTokenDestroy(token);
}

TEST(UfsrvFutureTest, BlockingGetWithCancellationRace) {
    const int iterations = 200;
    for (int it = 0; it < iterations; it++) {
        UfsrvFuture *future = nullptr;
        UfsrvPromise *promise = UfsrvPromiseCreate(&future);
        ASSERT_NE(promise, nullptr);
        UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
        ASSERT_NE(token, nullptr);

        std::thread completer([promise, it]() {
            int *v = static_cast<int *>(malloc(sizeof(int)));
            *v = it;
            UfsrvPromiseSetValue(promise, v, free);
        });
        std::thread canceller([token]() {
            UfsrvCancellationTokenCancel(token);
        });

        UfsrvFutureResult r = UfsrvFutureGetWithCancellation(future, token);
        completer.join();
        canceller.join();

        EXPECT_TRUE(r.error == 0 || r.error == ECANCELED);
        if (r.error == 0) {
            ASSERT_NE(r.value, nullptr);
            EXPECT_EQ(*static_cast<int *>(r.value), it);
        }

        UfsrvFutureRelease(future);
        UfsrvCancellationTokenDestroy(token);
    }
}

/* ── Cancellation ────────────────────────────────────────────────────────── */

TEST(UfsrvCancellationTokenTest, Basic) {
    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    ASSERT_NE(token, nullptr);
    EXPECT_FALSE(UfsrvCancellationTokenIsCancelled(token));

    UfsrvCancellationTokenCancel(token);
    EXPECT_TRUE(UfsrvCancellationTokenIsCancelled(token));
    UfsrvCancellationTokenCancel(token);  // idempotent
    EXPECT_TRUE(UfsrvCancellationTokenIsCancelled(token));

    UfsrvCancellationTokenDestroy(token);
}

TEST(UfsrvFutureTest, GetWithCancellationReturnsECanceled) {
    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    ASSERT_NE(token, nullptr);
    UfsrvCancellationTokenCancel(token);

    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    ASSERT_NE(promise, nullptr);

    // Cancelled token → ECANCELED immediately (no yield).
    UfsrvFutureResult result = UfsrvFutureGetWithCancellation(future, token);
    EXPECT_EQ(result.error, ECANCELED);

    UfsrvCancellationTokenDestroy(token);
    UfsrvPromiseDestroy(promise);
    UfsrvFutureRelease(future);
}

TEST(UfsrvFutureTest, CancelResumesSuspendedWaiter) {
    UfsrvScheduler *s1 = UfsrvSchedulerCreate("cg1");
    ASSERT_NE(s1, nullptr);
    ASSERT_EQ(UfsrvSchedulerStart(s1), 0);

    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    ASSERT_NE(token, nullptr);

    CancelGetContext ctx{nullptr, token, 0, 0, 0, -1};
    UfsrvCoroutineSpawn(s1, CancelGetEntry, &ctx);

    for (int i = 0; i < 10000 && ctx.reached.load() < 1; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_EQ(ctx.reached.load(), 1);

    /* Let the coroutine actually suspend, then cancel. */
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    UfsrvCancellationTokenCancel(token);

    for (int i = 0; i < 10000 && ctx.done.load() < 1; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    UfsrvSchedulerStop(s1);
    UfsrvSchedulerJoin(s1);
    UfsrvSchedulerDestroy(s1);

    EXPECT_EQ(ctx.done.load(), 1);
    EXPECT_EQ(ctx.error.load(), ECANCELED);

    UfsrvCancellationTokenDestroy(token);
}

TEST(UfsrvFutureTest, CancellationTokenLateCancelAfterComplete) {
    UfsrvScheduler *s1 = UfsrvSchedulerCreate("cg2");
    UfsrvScheduler *s2 = UfsrvSchedulerCreate("cg3");
    ASSERT_NE(s1, nullptr);
    ASSERT_NE(s2, nullptr);
    ASSERT_EQ(UfsrvSchedulerStart(s1), 0);
    ASSERT_EQ(UfsrvSchedulerStart(s2), 0);

    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    ASSERT_NE(token, nullptr);

    CancelGetContext ctx{s2, token, 0, 0, 0, -1};
    UfsrvCoroutineSpawn(s1, CancelGetEntry, &ctx);

    for (int i = 0; i < 10000 && ctx.done.load() < 1; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    /* Cancel after the waiter already resumed with the value — must be harmless. */
    UfsrvCancellationTokenCancel(token);

    UfsrvSchedulerStop(s1);
    UfsrvSchedulerJoin(s1);
    UfsrvSchedulerDestroy(s1);
    UfsrvSchedulerStop(s2);
    UfsrvSchedulerJoin(s2);
    UfsrvSchedulerDestroy(s2);

    EXPECT_EQ(ctx.done.load(), 1);
    EXPECT_EQ(ctx.error.load(), 0);
    EXPECT_EQ(ctx.value.load(), 42);

    UfsrvCancellationTokenDestroy(token);
}

TEST(UfsrvFutureTest, CancelVsCompleteRace) {
    UfsrvScheduler *s1 = UfsrvSchedulerCreate("cr1");
    ASSERT_NE(s1, nullptr);
    ASSERT_EQ(UfsrvSchedulerStart(s1), 0);

    const int iterations = 200;
    for (int it = 0; it < iterations; it++) {
        UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
        ASSERT_NE(token, nullptr);

        UfsrvFuture *future = nullptr;
        UfsrvPromise *promise = UfsrvPromiseCreate(&future);
        ASSERT_NE(promise, nullptr);

        RaceContext ctx{future, token, 0, 0, 0, -1};
        UfsrvCoroutineSpawn(s1, RaceGetEntry, &ctx);

        for (int i = 0; i < 10000 && ctx.reached.load() < 1; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(ctx.reached.load(), 1);

        /* Adversarial: cancel on one thread, complete on another. */
        std::thread canceller([token]() { UfsrvCancellationTokenCancel(token); });
        int *v = static_cast<int *>(malloc(sizeof(int)));
        *v = 42;
        UfsrvPromiseSetValue(promise, v, free);
        canceller.join();

        for (int i = 0; i < 10000 && ctx.done.load() < 1; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(ctx.done.load(), 1);
        EXPECT_TRUE(ctx.error.load() == 0 || ctx.error.load() == ECANCELED);
        if (ctx.error.load() == 0) {
            EXPECT_EQ(ctx.value.load(), 42);
        }

        UfsrvFutureRelease(future);
        UfsrvCancellationTokenDestroy(token);
    }

    UfsrvSchedulerStop(s1);
    UfsrvSchedulerJoin(s1);
    UfsrvSchedulerDestroy(s1);
}

TEST(UfsrvFutureTest, AttachCancellationCompletesFuture) {
    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    ASSERT_NE(promise, nullptr);

    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    ASSERT_NE(token, nullptr);

    ASSERT_TRUE(UfsrvFutureAttachCancellation(future, token));
    EXPECT_FALSE(UfsrvFutureIsReady(future));

    UfsrvCancellationTokenCancel(token);
    EXPECT_TRUE(UfsrvFutureIsReady(future));

    UfsrvFutureResult r = UfsrvFutureGet(future);   /* ready → fast path, no coroutine */
    EXPECT_EQ(r.error, ECANCELED);
    EXPECT_EQ(r.value, nullptr);

    UfsrvPromiseDestroy(promise);
    UfsrvFutureRelease(future);
    UfsrvCancellationTokenDestroy(token);
}

TEST(UfsrvFutureTest, AttachCancellationRacesPromise) {
    const int iterations = 200;
    for (int it = 0; it < iterations; it++) {
        UfsrvFuture *future = nullptr;
        UfsrvPromise *promise = UfsrvPromiseCreate(&future);
        ASSERT_NE(promise, nullptr);

        UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
        ASSERT_NE(token, nullptr);
        ASSERT_TRUE(UfsrvFutureAttachCancellation(future, token));

        int *v = static_cast<int *>(malloc(sizeof(int)));
        *v = 42;
        std::thread canceller([token]() { UfsrvCancellationTokenCancel(token); });
        UfsrvPromiseSetValue(promise, v, free);
        canceller.join();

        EXPECT_TRUE(UfsrvFutureIsReady(future));
        UfsrvFutureResult r = UfsrvFutureGet(future);
        EXPECT_TRUE(r.error == 0 || r.error == ECANCELED);
        if (r.error == 0) {
            ASSERT_NE(r.value, nullptr);
            EXPECT_EQ(*static_cast<int *>(r.value), 42);
        }

        UfsrvFutureRelease(future);
        UfsrvCancellationTokenDestroy(token);
    }
}

TEST(UfsrvFutureTest, MapInheritsAttachedToken) {
    UfsrvFuture *src = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&src);
    ASSERT_NE(promise, nullptr);

    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    ASSERT_NE(token, nullptr);
    ASSERT_TRUE(UfsrvFutureAttachCancellation(src, token));

    UfsrvFuture *mapped = UfsrvFutureMap(src, DoubleMapper, nullptr);
    ASSERT_NE(mapped, nullptr);

    UfsrvCancellationTokenCancel(token);
    EXPECT_TRUE(UfsrvFutureIsReady(src));
    EXPECT_TRUE(UfsrvFutureIsReady(mapped));
    EXPECT_EQ(UfsrvFutureGet(mapped).error, ECANCELED);

    UfsrvPromiseDestroy(promise);
    UfsrvFutureRelease(mapped);
    UfsrvFutureRelease(src);
    UfsrvCancellationTokenDestroy(token);
}

TEST(UfsrvFutureTest, LateCancelAfterReleaseIsSafe) {
    /* The future is retained by the cancel callback, so cancelling after the caller
     * releases it must remain safe (no use-after-free — ASan/LSan verify). */
    for (int it = 0; it < 100; it++) {
        UfsrvFuture *future = nullptr;
        UfsrvPromise *promise = UfsrvPromiseCreate(&future);
        ASSERT_NE(promise, nullptr);

        UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
        ASSERT_NE(token, nullptr);
        ASSERT_TRUE(UfsrvFutureAttachCancellation(future, token));

        UfsrvPromiseDestroy(promise);   /* abandon */
        UfsrvFutureRelease(future);     /* drop the caller reference */

        UfsrvCancellationTokenCancel(token);   /* cancel-after-release */
        UfsrvCancellationTokenDestroy(token);
    }
}

TEST(UfsrvFutureTest, AttachAfterCancelCompletesFuture) {
    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    ASSERT_NE(promise, nullptr);

    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    ASSERT_NE(token, nullptr);

    UfsrvCancellationTokenCancel(token);                  /* cancel first */
    ASSERT_TRUE(UfsrvFutureAttachCancellation(future, token));   /* attach after */

    EXPECT_TRUE(UfsrvFutureIsReady(future));
    EXPECT_EQ(UfsrvFutureGet(future).error, ECANCELED);

    UfsrvPromiseDestroy(promise);
    UfsrvFutureRelease(future);
    UfsrvCancellationTokenDestroy(token);
}

TEST(UfsrvFutureTest, FlatMapPropagatesCancellationToInner) {
    UfsrvFuture *inner = nullptr;
    UfsrvPromise *inner_promise = UfsrvPromiseCreate(&inner);
    ASSERT_NE(inner_promise, nullptr);

    UfsrvFuture *src = nullptr;
    UfsrvPromise *src_promise = UfsrvPromiseCreate(&src);
    ASSERT_NE(src_promise, nullptr);

    UfsrvCancellationToken *token = UfsrvCancellationTokenCreate();
    ASSERT_NE(token, nullptr);
    ASSERT_TRUE(UfsrvFutureAttachCancellation(src, token));

    FlatMapCancelContext fctx{inner};
    UfsrvFuture *flat = UfsrvFutureFlatMap(src, ReturnInner, &fctx);
    ASSERT_NE(flat, nullptr);

    /* Complete src → the mapper runs inline and attaches the token to `inner`. */
    int *v = static_cast<int *>(malloc(sizeof(int)));
    *v = 7;
    UfsrvPromiseSetValue(src_promise, v, free);

    EXPECT_FALSE(UfsrvFutureIsReady(inner));   /* producer hasn't completed it yet */

    UfsrvCancellationTokenCancel(token);

    /* The inner future is cancelled (completed), not left pending. Its result was
     * moved by the flatMap, so we assert readiness, not the (zeroed) error code. */
    EXPECT_TRUE(UfsrvFutureIsReady(inner));
    EXPECT_TRUE(UfsrvFutureIsReady(flat));
    EXPECT_EQ(UfsrvFutureGet(flat).error, ECANCELED);

    UfsrvFutureRelease(flat);
    UfsrvFutureRelease(src);
    UfsrvFutureRelease(inner);
    UfsrvPromiseDestroy(inner_promise);
    UfsrvCancellationTokenDestroy(token);
}

/* ── Null / lifetime ──────────────────────────────────────────────────────── */

TEST(UfsrvFutureTest, NullArguments) {
    EXPECT_EQ(UfsrvPromiseCreate(nullptr), nullptr);

    UfsrvPromiseSetValue(nullptr, nullptr, nullptr);
    UfsrvPromiseSetError(nullptr, 0);
    UfsrvPromiseDestroy(nullptr);

    EXPECT_FALSE(UfsrvFutureThen(nullptr, OnDone, nullptr));
    EXPECT_FALSE(UfsrvFutureThen(nullptr, nullptr, nullptr));
    EXPECT_FALSE(UfsrvFutureIsReady(nullptr));

    UfsrvFuture *error_a = UfsrvFutureFromError(0);
    UfsrvFuture *error_b = UfsrvFutureFromError(0);
    ASSERT_NE(error_a, nullptr);
    ASSERT_NE(error_b, nullptr);
    EXPECT_NE(error_a, error_b);
    UfsrvFutureRelease(error_a);
    UfsrvFutureRelease(error_b);

    EXPECT_EQ(UfsrvFutureMap(nullptr, DoubleMapper, nullptr), nullptr);
    EXPECT_EQ(UfsrvFutureFlatMap(nullptr, AsyncDouble, nullptr), nullptr);
    EXPECT_EQ(UfsrvFutureOnError(nullptr, Recover, nullptr), nullptr);
    EXPECT_EQ(UfsrvFutureDoOnSuccess(nullptr, RecordAction, nullptr), nullptr);

    UfsrvFutureRetain(nullptr);
    UfsrvFutureRelease(nullptr);
}

TEST(UfsrvFutureTest, AbandonNeverCompletes) {
    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    ASSERT_NE(promise, nullptr);

    UfsrvPromiseDestroy(promise);
    EXPECT_FALSE(UfsrvFutureIsReady(future));

    UfsrvFutureRelease(future);
}

TEST(UfsrvFutureTest, RetainReleaseBalance) {
    UfsrvFuture *future = nullptr;
    UfsrvPromise *promise = UfsrvPromiseCreate(&future);
    ASSERT_NE(promise, nullptr);

    UfsrvFutureRetain(future);
    UfsrvFutureRelease(future);
    UfsrvPromiseSetValue(promise, nullptr, nullptr);

    UfsrvFutureRelease(future);
}
