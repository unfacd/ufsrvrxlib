/**
 * @file ufsrv_scheduler_tests.cpp
 * @brief Adversarial gtest suite for the ufsrv_scheduler module.
 *
 * Exercises lifecycle edge cases, NULL handling, re-entrancy, concurrent
 * producers, and a high-throughput smoke. Assertions target invariants
 * (all accepted jobs run, no crash/leak) rather than thread placement.
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>

namespace {

/*! Callback that bumps an atomic counter. */
void CountJob(void *context_ptr) {
    auto *count = static_cast<std::atomic<int> *>(context_ptr);
    count->fetch_add(1, std::memory_order_relaxed);
}

/*! Context for WorkerProbeJob. */
struct WorkerProbe {
    UfsrvScheduler *scheduler;
    std::atomic<bool> is_worker{false};
    std::atomic<int> ran{0};
};

/*! Callback that records whether it ran on the scheduler's worker thread. */
void WorkerProbeJob(void *context_ptr) {
    auto *probe = static_cast<WorkerProbe *>(context_ptr);
    probe->is_worker.store(UfsrvSchedulerIsWorkerThread(probe->scheduler), std::memory_order_relaxed);
    probe->ran.fetch_add(1, std::memory_order_relaxed);
}

/*! Context for ReentrantJob. */
struct ReentrantCtx {
    UfsrvScheduler *scheduler;
    std::atomic<int> ran{0};
};

/*! Callback that re-submits itself once (exercises submit-from-worker). */
void ReentrantJob(void *context_ptr) {
    auto *ctx = static_cast<ReentrantCtx *>(context_ptr);
    if (ctx->ran.fetch_add(1, std::memory_order_relaxed) == 0) {
        UfsrvSchedulerSubmit(ctx->scheduler, ReentrantJob, ctx);
    }
}

}  // namespace

/* ── Single scheduler ─────────────────────────────────────────────────────── */

TEST(UfsrvSchedulerTest, CreateDestroyUnstarted) {
    UfsrvScheduler *s = UfsrvSchedulerCreate("unstarted");
    ASSERT_NE(s, nullptr);
    UfsrvSchedulerDestroy(s);

    UfsrvScheduler *no_name = UfsrvSchedulerCreate(nullptr);
    ASSERT_NE(no_name, nullptr);
    UfsrvSchedulerDestroy(no_name);
}

TEST(UfsrvSchedulerTest, NullArguments) {
    EXPECT_EQ(UfsrvSchedulerStart(nullptr), -1);
    EXPECT_FALSE(UfsrvSchedulerSubmit(nullptr, CountJob, nullptr));
    UfsrvSchedulerStop(nullptr);
    UfsrvSchedulerJoin(nullptr);
    UfsrvSchedulerDestroy(nullptr);
    EXPECT_FALSE(UfsrvSchedulerIsWorkerThread(nullptr));

    UfsrvScheduler *s = UfsrvSchedulerCreate(nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(UfsrvSchedulerSubmit(s, nullptr, nullptr));
    UfsrvSchedulerDestroy(s);
}

TEST(UfsrvSchedulerTest, Lifecycle) {
    UfsrvScheduler *s = UfsrvSchedulerCreate("life");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(UfsrvSchedulerStart(s), 0);
    EXPECT_EQ(UfsrvSchedulerStart(s), -1);  // second start rejected

    std::atomic<int> count{0};
    const int kJobs = 1000;
    for (int i = 0; i < kJobs; i++) {
        ASSERT_TRUE(UfsrvSchedulerSubmit(s, CountJob, &count));
    }

    UfsrvSchedulerStop(s);
    UfsrvSchedulerJoin(s);
    UfsrvSchedulerDestroy(s);

    EXPECT_EQ(count.load(), kJobs);
}

TEST(UfsrvSchedulerTest, SubmitBeforeStartRejected) {
    UfsrvScheduler *s = UfsrvSchedulerCreate(nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(UfsrvSchedulerSubmit(s, CountJob, nullptr));
    UfsrvSchedulerDestroy(s);
}

TEST(UfsrvSchedulerTest, SubmitAfterStopRejected) {
    UfsrvScheduler *s = UfsrvSchedulerCreate(nullptr);
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(UfsrvSchedulerStart(s), 0);
    UfsrvSchedulerStop(s);
    UfsrvSchedulerJoin(s);
    EXPECT_FALSE(UfsrvSchedulerSubmit(s, CountJob, nullptr));
    UfsrvSchedulerDestroy(s);
}

TEST(UfsrvSchedulerTest, DoubleStopAndJoin) {
    UfsrvScheduler *s = UfsrvSchedulerCreate(nullptr);
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(UfsrvSchedulerStart(s), 0);
    UfsrvSchedulerStop(s);
    UfsrvSchedulerStop(s);  // idempotent
    UfsrvSchedulerJoin(s);
    UfsrvSchedulerJoin(s);  // idempotent
    UfsrvSchedulerDestroy(s);
}

TEST(UfsrvSchedulerTest, DestroyWhileRunningDrainsJobs) {
    UfsrvScheduler *s = UfsrvSchedulerCreate(nullptr);
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(UfsrvSchedulerStart(s), 0);

    std::atomic<int> count{0};
    const int kJobs = 5000;
    for (int i = 0; i < kJobs; i++) {
        ASSERT_TRUE(UfsrvSchedulerSubmit(s, CountJob, &count));
    }

    UfsrvSchedulerDestroy(s);  // auto stop+join+drain
    EXPECT_EQ(count.load(), kJobs);
}

TEST(UfsrvSchedulerTest, IsWorkerThread) {
    UfsrvScheduler *s = UfsrvSchedulerCreate(nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_FALSE(UfsrvSchedulerIsWorkerThread(s));  // not started yet

    ASSERT_EQ(UfsrvSchedulerStart(s), 0);
    EXPECT_FALSE(UfsrvSchedulerIsWorkerThread(s));  // main thread is not the worker

    WorkerProbe probe{s, false, 0};
    ASSERT_TRUE(UfsrvSchedulerSubmit(s, WorkerProbeJob, &probe));

    UfsrvSchedulerStop(s);
    UfsrvSchedulerJoin(s);
    UfsrvSchedulerDestroy(s);

    EXPECT_EQ(probe.ran.load(), 1);
    EXPECT_TRUE(probe.is_worker.load());
}

TEST(UfsrvSchedulerTest, ReentrantSubmit) {
    UfsrvScheduler *s = UfsrvSchedulerCreate(nullptr);
    ASSERT_NE(s, nullptr);
    ASSERT_EQ(UfsrvSchedulerStart(s), 0);

    ReentrantCtx ctx{s, 0};
    ASSERT_TRUE(UfsrvSchedulerSubmit(s, ReentrantJob, &ctx));

    // Wait for the re-submitted job to run before stopping (bounded, to avoid a hang).
    for (int i = 0; i < 10000 && ctx.ran.load() < 2; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    UfsrvSchedulerStop(s);
    UfsrvSchedulerJoin(s);
    UfsrvSchedulerDestroy(s);

    EXPECT_EQ(ctx.ran.load(), 2);  // original + one re-submit
}

/* ── Scheduler pool ───────────────────────────────────────────────────────── */

TEST(UfsrvSchedulerPoolTest, CreateInvalid) {
    EXPECT_EQ(UfsrvSchedulerPoolCreate(0, nullptr), nullptr);
    EXPECT_EQ(UfsrvSchedulerPoolCreate(-1, nullptr), nullptr);
}

TEST(UfsrvSchedulerPoolTest, NullArguments) {
    EXPECT_EQ(UfsrvSchedulerPoolStart(nullptr), -1);
    EXPECT_FALSE(UfsrvSchedulerPoolSubmit(nullptr, CountJob, nullptr));
    EXPECT_FALSE(UfsrvSchedulerPoolSubmitTo(nullptr, 0, CountJob, nullptr));
    UfsrvSchedulerPoolStop(nullptr);
    UfsrvSchedulerPoolJoin(nullptr);
    UfsrvSchedulerPoolDestroy(nullptr);
    EXPECT_EQ(UfsrvSchedulerPoolSize(nullptr), 0);
}

TEST(UfsrvSchedulerPoolTest, PoolSize) {
    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate(4, nullptr);
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(UfsrvSchedulerPoolSize(pool), 4);
    UfsrvSchedulerPoolDestroy(pool);
}

TEST(UfsrvSchedulerPoolTest, Lifecycle) {
    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate(4, "pool");
    ASSERT_NE(pool, nullptr);
    EXPECT_EQ(UfsrvSchedulerPoolStart(pool), 0);

    std::atomic<int> count{0};
    const int kJobs = 4000;
    for (int i = 0; i < kJobs; i++) {
        ASSERT_TRUE(UfsrvSchedulerPoolSubmit(pool, CountJob, &count));
    }

    UfsrvSchedulerPoolStop(pool);
    UfsrvSchedulerPoolJoin(pool);
    UfsrvSchedulerPoolDestroy(pool);

    EXPECT_EQ(count.load(), kJobs);
}

TEST(UfsrvSchedulerPoolTest, SubmitToOutOfRangeFallsBack) {
    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate(2, nullptr);
    ASSERT_NE(pool, nullptr);
    ASSERT_EQ(UfsrvSchedulerPoolStart(pool), 0);

    std::atomic<int> count{0};
    ASSERT_TRUE(UfsrvSchedulerPoolSubmitTo(pool, -1, CountJob, &count));
    ASSERT_TRUE(UfsrvSchedulerPoolSubmitTo(pool, 99, CountJob, &count));
    ASSERT_TRUE(UfsrvSchedulerPoolSubmitTo(pool, 0, CountJob, &count));

    UfsrvSchedulerPoolStop(pool);
    UfsrvSchedulerPoolJoin(pool);
    UfsrvSchedulerPoolDestroy(pool);

    EXPECT_EQ(count.load(), 3);
}

TEST(UfsrvSchedulerPoolTest, SubmitBeforeStartRejected) {
    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate(2, nullptr);
    ASSERT_NE(pool, nullptr);
    EXPECT_FALSE(UfsrvSchedulerPoolSubmit(pool, CountJob, nullptr));
    UfsrvSchedulerPoolDestroy(pool);
}

TEST(UfsrvSchedulerPoolTest, ConcurrentProducers) {
    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate(4, "cp");
    ASSERT_NE(pool, nullptr);
    ASSERT_EQ(UfsrvSchedulerPoolStart(pool), 0);

    const int kProducers = 8;
    const int kPerProducer = 2000;
    std::atomic<int> count{0};

    std::vector<std::thread> threads;
    threads.reserve(kProducers);
    for (int p = 0; p < kProducers; p++) {
        threads.emplace_back([pool, &count] {
            for (int i = 0; i < kPerProducer; i++) {
                UfsrvSchedulerPoolSubmit(pool, CountJob, &count);
            }
        });
    }
    for (auto &t : threads) {
        t.join();
    }

    UfsrvSchedulerPoolStop(pool);
    UfsrvSchedulerPoolJoin(pool);
    UfsrvSchedulerPoolDestroy(pool);

    EXPECT_EQ(count.load(), kProducers * kPerProducer);
}

TEST(UfsrvSchedulerPoolTest, HighThroughput) {
    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate(8, "ht");
    ASSERT_NE(pool, nullptr);
    ASSERT_EQ(UfsrvSchedulerPoolStart(pool), 0);

    std::atomic<int> count{0};
    const int kJobs = 100000;
    for (int i = 0; i < kJobs; i++) {
        UfsrvSchedulerPoolSubmit(pool, CountJob, &count);
    }

    UfsrvSchedulerPoolStop(pool);
    UfsrvSchedulerPoolJoin(pool);
    UfsrvSchedulerPoolDestroy(pool);

    EXPECT_EQ(count.load(), kJobs);
}

/* ── Describe / List introspection ─────────────────────────────────────────── */

TEST(UfsrvSchedulerPoolTest, ListSchedulerWorkers) {
    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate(3, "ws");
    ASSERT_NE(pool, nullptr);

    CollectionDescriptor *cd = ListSchedulerWorkers(pool);
    ASSERT_NE(cd, nullptr);
    EXPECT_EQ(cd->collection_sz, 3u);
    EXPECT_EQ(cd->collection_base_offset, 0u);
    for (size_t i = 0; i < cd->collection_sz; i++) {
        const char *name = static_cast<const char *>(cd->collection[i]);
        ASSERT_NE(name, nullptr);
        EXPECT_EQ(std::string(name), "ws-" + std::to_string(i));
    }
    free(cd);  // exactly one free releases the whole slab

    UfsrvSchedulerPoolDestroy(pool);
}

TEST(UfsrvSchedulerPoolTest, DescribeSchedulerAllWorkers) {
    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate(3, "ws");
    ASSERT_NE(pool, nullptr);
    ASSERT_EQ(UfsrvSchedulerPoolStart(pool), 0);

    BufferDescriptor *bd = DescribeScheduler(pool, nullptr, nullptr);
    ASSERT_NE(bd, nullptr);
    ASSERT_NE(bd->data, nullptr);
    const std::string json(bd->data);

    EXPECT_NE(json.find("\"worker_pool_sz\":3"), std::string::npos);
    EXPECT_NE(json.find("ws-0"), std::string::npos);
    EXPECT_NE(json.find("ws-1"), std::string::npos);
    EXPECT_NE(json.find("ws-2"), std::string::npos);

    BufferDescriptorRelease(bd);
    free(bd);

    UfsrvSchedulerPoolStop(pool);
    UfsrvSchedulerPoolJoin(pool);
    UfsrvSchedulerPoolDestroy(pool);
}

TEST(UfsrvSchedulerPoolTest, DescribeSchedulerSpecificWorker) {
    UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate(3, "ws");
    ASSERT_NE(pool, nullptr);
    ASSERT_EQ(UfsrvSchedulerPoolStart(pool), 0);

    BufferDescriptor *bd = DescribeScheduler(pool, "ws-1", nullptr);
    ASSERT_NE(bd, nullptr);
    const std::string json(bd->data);

    EXPECT_NE(json.find("ws-1"), std::string::npos);
    EXPECT_EQ(json.find("ws-0"), std::string::npos);
    EXPECT_EQ(json.find("ws-2"), std::string::npos);

    BufferDescriptorRelease(bd);
    free(bd);

    UfsrvSchedulerPoolStop(pool);
    UfsrvSchedulerPoolJoin(pool);
    UfsrvSchedulerPoolDestroy(pool);
}

TEST(UfsrvSchedulerPoolTest, DescribeSchedulerNullPoolAndProvided) {
    // NULL pool → empty document, still a well-formed JSON object.
    BufferDescriptor *bd = DescribeScheduler(nullptr, nullptr, nullptr);
    ASSERT_NE(bd, nullptr);
    EXPECT_NE(std::string(bd->data).find("\"workers\":[]"), std::string::npos);
    BufferDescriptorRelease(bd);
    free(bd);

    // Provided descriptor is reused (same pointer returned) and populated.
    BufferDescriptor provided;
    BufferDescriptorInit(&provided, 64);
    BufferDescriptor *ret = DescribeScheduler(nullptr, nullptr, &provided);
    EXPECT_EQ(ret, &provided);
    EXPECT_NE(std::string(provided.data).find("\"workers\":[]"), std::string::npos);
    BufferDescriptorRelease(&provided);

    // NULL pool → ListSchedulerWorkers returns NULL.
    EXPECT_EQ(ListSchedulerWorkers(nullptr), nullptr);
}
