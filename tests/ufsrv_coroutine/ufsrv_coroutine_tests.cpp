/**
 * @file ufsrv_coroutine_tests.cpp
 * @brief Basic gtest suite for the ufsrv_coroutine module (libaco wrapper).
 */
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include <ufsrvrxlib/ufsrv_coroutine/ufsrv_coroutine.h>
#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>

namespace {

int g_step = 0;
int g_arg = 0;
UfsrvCoroutine *g_current_in_co = nullptr;
UfsrvCoroutine *g_created_co = nullptr;

/*! Entry: record arg, yield once, then exit. */
void StepEntry(void) {
    g_arg = *static_cast<int *>(UfsrvCoroutineGetArg());
    g_step = 1;
    UfsrvCoroutineYield();
    g_step = 2;
    UfsrvCoroutineExit();
}

/*! Entry: record the current coroutine handle. */
void CurrentEntry(void) {
    g_current_in_co = UfsrvCoroutineCurrent();
    UfsrvCoroutineExit();
}

/*! Entry: never exits (yields forever) — to check repeated resume. */
void YieldingEntry(void) {
    for (int i = 0; i < 3; i++) {
        g_step = i + 1;
        UfsrvCoroutineYield();
    }
    UfsrvCoroutineExit();
}

/*! Context for the scheduler-integration coroutine. */
struct CoCtx {
    UfsrvScheduler *scheduler;
    std::atomic<int> step{0};
};

/*! Entry: re-submit self on the worker, yield, then record completion. */
void IntegrationEntry(void) {
    auto *ctx = static_cast<CoCtx *>(UfsrvCoroutineGetArg());
    ctx->step.store(1, std::memory_order_relaxed);
    UfsrvCoroutineSubmitResume(ctx->scheduler, UfsrvCoroutineCurrent());
    UfsrvCoroutineYield();
    ctx->step.store(2, std::memory_order_relaxed);
    UfsrvCoroutineExit();
}

}  // namespace

TEST(UfsrvCoroutineTest, CreateResumeYieldExit) {
    int value = 99;
    g_step = 0;
    g_arg = 0;

    UfsrvCoroutine *co = UfsrvCoroutineCreate(StepEntry, &value);
    ASSERT_NE(co, nullptr);
    EXPECT_EQ(g_step, 0);

    UfsrvCoroutineResume(co);   // runs to the first yield
    EXPECT_EQ(g_step, 1);
    EXPECT_EQ(g_arg, 99);

    UfsrvCoroutineResume(co);   // resumes, runs to exit (and is destroyed)
    EXPECT_EQ(g_step, 2);
}

TEST(UfsrvCoroutineTest, ArgPassing) {
    int value = 7;
    g_arg = 0;
    g_step = 0;

    UfsrvCoroutine *co = UfsrvCoroutineCreate(StepEntry, &value);
    ASSERT_NE(co, nullptr);
    UfsrvCoroutineResume(co);
    EXPECT_EQ(g_arg, 7);
    UfsrvCoroutineResume(co);   // run to exit
    EXPECT_EQ(g_step, 2);
}

TEST(UfsrvCoroutineTest, CurrentHandle) {
    g_current_in_co = nullptr;

    UfsrvCoroutine *co = UfsrvCoroutineCreate(CurrentEntry, nullptr);
    ASSERT_NE(co, nullptr);
    UfsrvCoroutineResume(co);

    EXPECT_EQ(g_current_in_co, co);   // the running coroutine is the current handle
    EXPECT_NE(UfsrvCoroutineCurrent(), co);  // back on the main coroutine now
}

TEST(UfsrvCoroutineTest, RepeatedYield) {
    g_step = 0;

    UfsrvCoroutine *co = UfsrvCoroutineCreate(YieldingEntry, nullptr);
    ASSERT_NE(co, nullptr);

    UfsrvCoroutineResume(co);
    EXPECT_EQ(g_step, 1);
    UfsrvCoroutineResume(co);
    EXPECT_EQ(g_step, 2);
    UfsrvCoroutineResume(co);
    EXPECT_EQ(g_step, 3);
    UfsrvCoroutineResume(co);   // runs to exit
    EXPECT_EQ(g_step, 3);
}

TEST(UfsrvCoroutineTest, NullHandling) {
    EXPECT_EQ(UfsrvCoroutineCreate(nullptr, nullptr), nullptr);
    UfsrvCoroutineResume(nullptr);   // no-op
}

TEST(UfsrvCoroutineTest, ThreadCleanup) {
    // Force lazy init via create + resume.
    int value = 0;
    g_step = 0;
    UfsrvCoroutine *co = UfsrvCoroutineCreate(StepEntry, &value);
    ASSERT_NE(co, nullptr);
    UfsrvCoroutineResume(co);
    UfsrvCoroutineResume(co);   // exit (destroys the coroutine)

    UfsrvCoroutineThreadCleanup();
    UfsrvCoroutineThreadCleanup();   // idempotent

    // Re-init works after cleanup.
    co = UfsrvCoroutineCreate(StepEntry, &value);
    ASSERT_NE(co, nullptr);
    UfsrvCoroutineResume(co);
    EXPECT_EQ(g_step, 1);
    UfsrvCoroutineResume(co);   // exit
    EXPECT_EQ(g_step, 2);

    UfsrvCoroutineThreadCleanup();
}

TEST(UfsrvCoroutineTest, SchedulerIntegration) {
    UfsrvScheduler *scheduler = UfsrvSchedulerCreate("coro");
    ASSERT_NE(scheduler, nullptr);
    ASSERT_EQ(UfsrvSchedulerStart(scheduler), 0);

    CoCtx ctx{scheduler, 0};
    UfsrvCoroutineSpawn(scheduler, IntegrationEntry, &ctx);

    // Bounded wait for the coroutine to run to completion on the worker.
    for (int i = 0; i < 10000 && ctx.step.load() < 2; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    UfsrvSchedulerStop(scheduler);
    UfsrvSchedulerJoin(scheduler);
    UfsrvSchedulerDestroy(scheduler);

    EXPECT_EQ(ctx.step.load(), 2);
}
