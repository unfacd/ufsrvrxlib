/**
 * @file ufsrv_scheduler_type.h
 * @brief Public types for the ufsrv_scheduler module.
 */

#ifndef UFSRVRXLIB_UFSRV_SCHEDULER_TYPE_H
#define UFSRVRXLIB_UFSRV_SCHEDULER_TYPE_H

#include <uflib/standard_c_includes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*! Opaque handle to a single-worker scheduler. */
typedef struct UfsrvScheduler UfsrvScheduler;

/*! Opaque handle to a pool of schedulers. */
typedef struct UfsrvSchedulerPool UfsrvSchedulerPool;

/*!
 * Job callback invoked by a worker thread.
 *
 * @param[in] context_ptr  User context passed to the submit call.
 */
typedef void (*UfsrvSchedulerJobCallback)(void *context_ptr);

#ifdef __cplusplus
}
#endif

#endif /* UFSRVRXLIB_UFSRV_SCHEDULER_TYPE_H */
