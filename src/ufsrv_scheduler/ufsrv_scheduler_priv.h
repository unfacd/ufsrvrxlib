/**
 * @file ufsrv_scheduler_priv.h
 * @brief Private definitions for the ufsrv_scheduler implementation.
 *
 * Colocated with the implementation under src/ — never installed, never
 * included by consumers.
 */

#ifndef UFSRVRXLIB_UFSRV_SCHEDULER_PRIV_H
#define UFSRVRXLIB_UFSRV_SCHEDULER_PRIV_H

#include <pthread.h>
#include <stdatomic.h>

#include <uflib/cdt/cdt_mpsc_queue.h>

#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler_type.h>

/*!
 * A submitted job. The intrusive MPSC queue_node is embedded; the job pointer is
 * recovered on the consumer side via `queue_node.context_data`.
 */
struct UfsrvSchedulerJob
{
  struct mpsc_queue_node    queue_node;       /*!< Intrusive queue link. */
  UfsrvSchedulerJobCallback callback;         /*!< Work to run. */
  void *                    callback_context; /*!< Passed to the callback. */
};

/*!
 * Single-worker scheduler: one OS thread, one lock-free MPSC queue, one
 * eventfd used to wake the worker when work is submitted.
 */
struct UfsrvScheduler
{
  struct LocklessMpscQueue queue;
  int                      event_fd;
  pthread_t                thread;
  char *                   name;
  _Atomic(bool)            is_running;
  _Atomic(bool)            is_started;
  _Atomic(bool)            is_joined;
  _Atomic(bool)            is_worker_id_ready;
  pthread_t                worker_id;
};

/*!
 * Pool of schedulers; submits are distributed round-robin.
 */
struct UfsrvSchedulerPool
{
  UfsrvScheduler **workers;
  int              worker_pool_sz;
  _Atomic(int)     next;
  _Atomic(bool)    is_running;
};

#endif /* UFSRVRXLIB_UFSRV_SCHEDULER_PRIV_H */
