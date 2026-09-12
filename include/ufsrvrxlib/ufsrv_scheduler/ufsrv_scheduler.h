/**
 * @file ufsrv_scheduler.h
 * @brief Single-worker scheduler and a round-robin worker pool, each worker
 *        draining a lock-free MPSC queue and sleeping on an eventfd.
 *
 * Each UfsrvScheduler owns one OS thread and one intrusive lock-free MPSC
 * queue (uflib cdt_mpsc_queue). Any thread may submit a job; the worker pops
 * jobs and runs their callback, in submission order per worker.
 *
 * @code{.c}
 * UfsrvSchedulerPool *pool = UfsrvSchedulerPoolCreate(4, "ws");
 * UfsrvSchedulerPoolStart(pool);
 *
 * UfsrvSchedulerPoolSubmit(pool, my_job, my_context);
 *
 * UfsrvSchedulerPoolStop(pool);
 * UfsrvSchedulerPoolJoin(pool);
 * UfsrvSchedulerPoolDestroy(pool);
 * @endcode
 */

#ifndef UFSRVRXLIB_UFSRV_SCHEDULER_H
#define UFSRVRXLIB_UFSRV_SCHEDULER_H

#include <ufsrvrxlib/ufsrvrxlib_defs.h>
#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler_type.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <uflib/main_types.h>
#include <uflib/buffer_descriptor/buffer_descriptor.h>

/*!
 * @brief Create a single-worker scheduler (not yet started).
 *
 * @param[in] name_ptr  Optional thread name, copied; may be NULL.
 * @return A new UfsrvScheduler, or NULL on allocation failure.
 */
PUBLIC_API UfsrvScheduler *UfsrvSchedulerCreate(const char *name_ptr);

/*!
 * @brief Start the scheduler's worker thread.
 *
 * @param[in,out] scheduler_ptr  Scheduler to start.
 * @return 0 on success, -1 if already started or thread creation failed.
 */
PUBLIC_API int UfsrvSchedulerStart(UfsrvScheduler *scheduler_ptr);

/*!
 * @brief Submit a job to the scheduler.
 *
 * @param[in,out] scheduler_ptr  Target scheduler.
 * @param[in]     callback       Job function (non-NULL).
 * @param[in]     context_ptr    Opaque context passed to the callback.
 * @return true if accepted, false if not running or on allocation failure.
 */
PUBLIC_API bool UfsrvSchedulerSubmit(UfsrvScheduler *scheduler_ptr, UfsrvSchedulerJobCallback callback, void *context_ptr);

/*!
 * @brief Signal the worker to stop (does not wait).
 *
 * @param[in,out] scheduler_ptr  Scheduler to stop.
 */
PUBLIC_API void UfsrvSchedulerStop(UfsrvScheduler *scheduler_ptr);

/*!
 * @brief Wait for the worker thread to exit.
 *
 * @param[in,out] scheduler_ptr  Scheduler to join.
 */
PUBLIC_API void UfsrvSchedulerJoin(UfsrvScheduler *scheduler_ptr);

/*!
 * @brief Stop (if running), join, drain and release the scheduler.
 *
 * Any jobs still queued are freed without is_running their callback.
 *
 * @param[in,out] scheduler_ptr  Scheduler to destroy (may be NULL).
 */
PUBLIC_API void UfsrvSchedulerDestroy(UfsrvScheduler *scheduler_ptr);

/*!
 * @brief Report whether the calling thread is this scheduler's worker.
 *
 * @param[in] scheduler_ptr  Scheduler to test.
 * @return true if the current thread is the scheduler's worker thread.
 */
PUBLIC_API bool UfsrvSchedulerIsWorkerThread(const UfsrvScheduler *scheduler_ptr);

/*!
 * @brief Create a pool of schedulers (not yet started).
 *
 * @param[in] worker_count     Number of workers (> 0).
 * @param[in] name_prefix_ptr  Optional thread-name prefix, copied; may be NULL.
 * @return A new UfsrvSchedulerPool, or NULL on invalid input / allocation failure.
 */
PUBLIC_API UfsrvSchedulerPool *UfsrvSchedulerPoolCreate(int worker_count, const char *name_prefix_ptr);

/*!
 * @brief Start every worker in the pool.
 *
 * @param[in,out] pool_ptr  Pool to start.
 * @return 0 on success, -1 if any worker failed to start (already-started ones are stopped).
 */
PUBLIC_API int UfsrvSchedulerPoolStart(UfsrvSchedulerPool *pool_ptr);

/*!
 * @brief Submit a job to the pool, distributed round-robin.
 *
 * @param[in,out] pool_ptr     Target pool.
 * @param[in]     callback     Job function (non-NULL).
 * @param[in]     context_ptr  Opaque context passed to the callback.
 * @return true if accepted, false otherwise.
 */
PUBLIC_API bool UfsrvSchedulerPoolSubmit(UfsrvSchedulerPool *pool_ptr, UfsrvSchedulerJobCallback callback, void *context_ptr);

/*!
 * @brief Submit a job to a specific worker in the pool.
 *
 * Falls back to round-robin if worker_index is out of range.
 *
 * @param[in,out] pool_ptr     Target pool.
 * @param[in]     worker_index Worker index (0 .. size-1).
 * @param[in]     callback     Job function (non-NULL).
 * @param[in]     context_ptr  Opaque context passed to the callback.
 * @return true if accepted, false otherwise.
 */
PUBLIC_API bool UfsrvSchedulerPoolSubmitTo(UfsrvSchedulerPool *pool_ptr, int worker_index, UfsrvSchedulerJobCallback callback, void *context_ptr);

/*!
 * @brief Signal all workers to stop (does not wait).
 *
 * @param[in,out] pool_ptr  Pool to stop.
 */
PUBLIC_API void UfsrvSchedulerPoolStop(UfsrvSchedulerPool *pool_ptr);

/*!
 * @brief Wait for all workers to exit.
 *
 * @param[in,out] pool_ptr  Pool to join.
 */
PUBLIC_API void UfsrvSchedulerPoolJoin(UfsrvSchedulerPool *pool_ptr);

/*!
 * @brief Stop (if running), join and release the pool and all its workers.
 *
 * @param[in,out] pool_ptr  Pool to destroy (may be NULL).
 */
PUBLIC_API void UfsrvSchedulerPoolDestroy(UfsrvSchedulerPool *pool_ptr);

/*!
 * @brief Number of workers in the pool.
 *
 * @param[in] pool_ptr  Pool to query.
 * @return Worker count, or 0 if pool_ptr is NULL.
 */
PUBLIC_API int UfsrvSchedulerPoolSize(const UfsrvSchedulerPool *pool_ptr);

/*!
 * @brief Describe the pool's workers as a JSON string (no json-c).
 *
 * Builds a JSON document describing the pool and each of its workers — lifecycle
 * flags, worker id, and the wake-up eventfd.  When @p worker_name is non-NULL,
 * only the worker whose name matches exactly is emitted; otherwise every worker
 * is described.
 *
 * The caller may supply a pre-initialised BufferDescriptor, or pass NULL to have
 * one allocated and initialised (256-byte initial capacity).  On allocation
 * failure NULL is returned.
 *
 * @param[in] pool_ptr     Pool to describe (NULL → empty "workers":[] document).
 * @param[in] worker_name  Exact worker name to filter on, or NULL for all workers.
 * @param[in,out] provided Caller-owned BufferDescriptor, or NULL to allocate.
 * @return The populated BufferDescriptor (the same pointer as @p provided when
 *         non-NULL), or NULL if a new descriptor could not be allocated.
 *
 * @code{.c}
 * BufferDescriptor bd;
 * BufferDescriptorInit(&bd, 512);
 * DescribeScheduler(pool, "ws-1", &bd);
 * printf("%s\n", bd.data);
 * BufferDescriptorRelease(&bd);
 * @endcode
 */
PUBLIC_API BufferDescriptor *
DescribeScheduler(UfsrvSchedulerPool *pool_ptr, const char *worker_name, BufferDescriptor *provided);

/*!
 * @brief List every worker name in one contiguous allocation.
 *
 * Returns a freshly allocated CollectionDescriptor whose `collection` array
 * holds one pointer per worker (in pool order), each pointing at that worker's
 * name.  The descriptor, the pointer array, and the name strings are all carved
 * from a single malloc block, so the caller releases everything with one
 * `free()`.
 *
 * `collection_base_offset` is always 0 (elements are referenced by pointer, not
 * by value).  Each `collection[i]` is read back as `(const char *)`.
 *
 * @param[in] pool_ptr  Pool whose worker names are listed (NULL → NULL).
 * @return Newly allocated CollectionDescriptor, or NULL on allocation failure.
 *         The caller owns it and must free() it exactly once when done.
 *
 * @code{.c}
 * CollectionDescriptor *workers = ListSchedulerWorkers(pool);
 * if (workers) {
 *     for (size_t i = 0; i < workers->collection_sz; i++) {
 *         const char *name = (const char *)workers->collection[i];
 *         // e.g. DescribeScheduler(pool, name, &bd);
 *     }
 *     free(workers);
 * }
 * @endcode
 */
PUBLIC_API CollectionDescriptor *
ListSchedulerWorkers(UfsrvSchedulerPool *pool_ptr);

#ifdef __cplusplus
}
#endif

#endif /* UFSRVRXLIB_UFSRV_SCHEDULER_H */
