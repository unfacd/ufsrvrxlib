/**
 * @file ufsrv_scheduler.c
 * @brief Single-worker scheduler and worker pool over uflib's lock-free MPSC queue.
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

#include <ufsrvrxlib/ufsrv_scheduler/ufsrv_scheduler.h>

#include "ufsrv_scheduler_priv.h"

#include <sys/eventfd.h>

/*!
 * @brief Run a dequeued job's callback and release the job.
 *
 * @param[in] node_ptr  Intrusive MPSC node whose context_data points at the job.
 */
static void
sRunJob(struct mpsc_queue_node *node_ptr)
{
  struct UfsrvSchedulerJob *job_ptr = node_ptr->context_data;

  if (job_ptr->callback != NULL) {
    job_ptr->callback(job_ptr->callback_context);
  }
  free(job_ptr);
}

/*!
 * @brief Worker thread entry: drain the MPSC queue, sleeping on eventfd.
 *
 * @param[in] arg_ptr  The UfsrvScheduler this thread serves.
 * @return Always NULL.
 */
static void *
sWorkerMain(void *arg_ptr)
{
  UfsrvScheduler *scheduler_ptr = arg_ptr;

  scheduler_ptr->worker_id = pthread_self();
  atomic_store_explicit(&scheduler_ptr->is_worker_id_ready, true, memory_order_release);

  if (scheduler_ptr->name != NULL) {
    pthread_setname_np(pthread_self(), scheduler_ptr->name);
  }

  uint64_t event_value;

  while (atomic_load_explicit(&scheduler_ptr->is_running, memory_order_acquire)) {
    struct mpsc_queue_node *node_ptr = mpsc_queue_pop(&scheduler_ptr->queue);
    if (node_ptr != NULL) {
      sRunJob(node_ptr);
      continue;
    }

    if (!atomic_load_explicit(&scheduler_ptr->is_running, memory_order_acquire)) {
      break;
    }

    ssize_t n = read(scheduler_ptr->event_fd, &event_value, sizeof(event_value));
    if (n != (ssize_t)sizeof(event_value) && errno != EINTR) {
      break;
    }
  }

  /* Final drain: run anything submitted before we observed is_running == false. */
  for (;;) {
    struct mpsc_queue_node *node_ptr = mpsc_queue_pop(&scheduler_ptr->queue);
    if (node_ptr == NULL) {
      break;
    }
    sRunJob(node_ptr);
  }

  return NULL;
}

UfsrvScheduler *
UfsrvSchedulerCreate(const char *name_ptr)
{
  UfsrvScheduler *scheduler_ptr = calloc(1, sizeof(*scheduler_ptr));
  if (scheduler_ptr == NULL) {
    return NULL;
  }

  if (name_ptr != NULL) {
    scheduler_ptr->name = strdup(name_ptr);
    if (scheduler_ptr->name == NULL) {
      free(scheduler_ptr);
      return NULL;
    }
  }

  scheduler_ptr->event_fd = eventfd(0, EFD_CLOEXEC);
  if (scheduler_ptr->event_fd < 0) {
    free(scheduler_ptr->name);
    free(scheduler_ptr);
    return NULL;
  }

  mpsc_queue_init(&scheduler_ptr->queue);

  atomic_init(&scheduler_ptr->is_running, false);
  atomic_init(&scheduler_ptr->is_started, false);
  atomic_init(&scheduler_ptr->is_joined, false);
  atomic_init(&scheduler_ptr->is_worker_id_ready, false);

  return scheduler_ptr;
}

int
UfsrvSchedulerStart(UfsrvScheduler *scheduler_ptr)
{
  if (scheduler_ptr == NULL) {
    return -1;
  }
  if (atomic_load_explicit(&scheduler_ptr->is_started, memory_order_acquire)) {
    return -1;
  }

  atomic_store_explicit(&scheduler_ptr->is_running, true, memory_order_release);

  if (pthread_create(&scheduler_ptr->thread, NULL, sWorkerMain, scheduler_ptr) != 0) {
    atomic_store_explicit(&scheduler_ptr->is_running, false, memory_order_release);
    return -1;
  }

  atomic_store_explicit(&scheduler_ptr->is_started, true, memory_order_release);
  return 0;
}

/*!
 * @brief Allocate a job, enqueue it and wake the worker.
 *
 * @param[in,out] scheduler_ptr  Target scheduler.
 * @param[in]     callback       Job function.
 * @param[in]     context_ptr    Opaque context passed to the callback.
 * @return true on success, false if not running or on allocation failure.
 */
static bool
sSubmitToScheduler(UfsrvScheduler *scheduler_ptr, UfsrvSchedulerJobCallback callback, void *context_ptr)
{
  struct UfsrvSchedulerJob *job_ptr = malloc(sizeof(*job_ptr));
  if (job_ptr == NULL) {
    return false;
  }

  job_ptr->callback                = callback;
  job_ptr->callback_context        = context_ptr;
  job_ptr->queue_node.context_data = job_ptr;

  if (!atomic_load_explicit(&scheduler_ptr->is_running, memory_order_acquire)) {
    free(job_ptr);
    return false;
  }

  mpsc_queue_insert(&scheduler_ptr->queue, &job_ptr->queue_node);

  uint64_t one = 1;
  write(scheduler_ptr->event_fd, &one, sizeof(one));
  return true;
}

bool
UfsrvSchedulerSubmit(UfsrvScheduler *scheduler_ptr, UfsrvSchedulerJobCallback callback, void *context_ptr)
{
  if (scheduler_ptr == NULL || callback == NULL) {
    return false;
  }
  return sSubmitToScheduler(scheduler_ptr, callback, context_ptr);
}

void
UfsrvSchedulerStop(UfsrvScheduler *scheduler_ptr)
{
  if (scheduler_ptr == NULL) {
    return;
  }

  atomic_store_explicit(&scheduler_ptr->is_running, false, memory_order_release);
  uint64_t one = 1;
  write(scheduler_ptr->event_fd, &one, sizeof(one));
}

void
UfsrvSchedulerJoin(UfsrvScheduler *scheduler_ptr)
{
  if (scheduler_ptr == NULL) {
    return;
  }
  if (!atomic_load_explicit(&scheduler_ptr->is_started, memory_order_acquire)) {
    return;
  }
  if (atomic_exchange_explicit(&scheduler_ptr->is_joined, true, memory_order_acq_rel)) {
    return;
  }
  pthread_join(scheduler_ptr->thread, NULL);
}

void
UfsrvSchedulerDestroy(UfsrvScheduler *scheduler_ptr)
{
  if (scheduler_ptr == NULL) {
    return;
  }

  if (atomic_load_explicit(&scheduler_ptr->is_started, memory_order_acquire) && !atomic_load_explicit( &scheduler_ptr->is_joined, memory_order_acquire)) {
    UfsrvSchedulerStop(scheduler_ptr);
    UfsrvSchedulerJoin(scheduler_ptr);
  }

  for (;;) {
    struct mpsc_queue_node *node_ptr = mpsc_queue_pop(&scheduler_ptr->queue);
    if (node_ptr == NULL) {
      break;
    }
    free(node_ptr->context_data);
  }

  if (scheduler_ptr->event_fd >= 0) {
    close(scheduler_ptr->event_fd);
  }
  free(scheduler_ptr->name);
  free(scheduler_ptr);
}

bool
UfsrvSchedulerIsWorkerThread(const UfsrvScheduler *scheduler_ptr)
{
  if (scheduler_ptr == NULL) {
    return false;
  }
  if (!atomic_load_explicit(&scheduler_ptr->is_worker_id_ready, memory_order_acquire)) {
    return false;
  }
  return pthread_equal(pthread_self(), scheduler_ptr->worker_id);
}

UfsrvSchedulerPool *
UfsrvSchedulerPoolCreate(int worker_count, const char *name_prefix_ptr)
{
  if (worker_count <= 0) {
    return NULL;
  }

  UfsrvSchedulerPool *pool_ptr = calloc(1, sizeof(*pool_ptr));
  if (pool_ptr == NULL) {
    return NULL;
  }

  pool_ptr->workers = calloc((size_t)worker_count, sizeof(*pool_ptr->workers));
  if (pool_ptr->workers == NULL) {
    free(pool_ptr);
    return NULL;
  }

  pool_ptr->worker_pool_sz = worker_count;
  atomic_init(&pool_ptr->next, 0);
  atomic_init(&pool_ptr->is_running, false);

  for (int i = 0; i < worker_count; i++) {
    char name[64];
    if (name_prefix_ptr != NULL) {
      snprintf(name, sizeof(name), "%s-%d", name_prefix_ptr, i);
    }
    else {
      snprintf(name, sizeof(name), "worker-%d", i);
    }

    pool_ptr->workers[i] = UfsrvSchedulerCreate(name);
    if (pool_ptr->workers[i] == NULL) {
      for (int j = 0; j < i; j++) {
        UfsrvSchedulerDestroy(pool_ptr->workers[j]);
      }
      free(pool_ptr->workers);
      free(pool_ptr);
      return NULL;
    }
  }

  return pool_ptr;
}

int
UfsrvSchedulerPoolStart(UfsrvSchedulerPool *pool_ptr)
{
  if (pool_ptr == NULL) {
    return -1;
  }

  for (int i = 0; i < pool_ptr->worker_pool_sz; i++) {
    if (UfsrvSchedulerStart(pool_ptr->workers[i]) != 0) {
      for (int j = 0; j < i; j++) {
        UfsrvSchedulerStop(pool_ptr->workers[j]);
      }
      return -1;
    }
  }

  atomic_store_explicit(&pool_ptr->is_running, true, memory_order_release);
  return 0;
}

bool
UfsrvSchedulerPoolSubmit(UfsrvSchedulerPool *pool_ptr, UfsrvSchedulerJobCallback callback, void *context_ptr)
{
  if (pool_ptr == NULL || callback == NULL) {
    return false;
  }
  if (!atomic_load_explicit(&pool_ptr->is_running, memory_order_acquire)) {
    return false;
  }

  int idx = atomic_fetch_add_explicit(&pool_ptr->next, 1, memory_order_relaxed) % pool_ptr->worker_pool_sz;
  return UfsrvSchedulerSubmit(pool_ptr->workers[idx], callback, context_ptr);
}

bool
UfsrvSchedulerPoolSubmitTo(UfsrvSchedulerPool *pool_ptr, int worker_index, UfsrvSchedulerJobCallback callback,
                           void *              context_ptr)
{
  if (pool_ptr == NULL || callback == NULL) {
    return false;
  }
  if (!atomic_load_explicit(&pool_ptr->is_running, memory_order_acquire)) {
    return false;
  }

  if (worker_index < 0 || worker_index >= pool_ptr->worker_pool_sz) {
    return UfsrvSchedulerPoolSubmit(pool_ptr, callback, context_ptr);
  }

  return UfsrvSchedulerSubmit(pool_ptr->workers[worker_index], callback, context_ptr);
}

void
UfsrvSchedulerPoolStop(UfsrvSchedulerPool *pool_ptr)
{
  if (pool_ptr == NULL) {
    return;
  }

  atomic_store_explicit(&pool_ptr->is_running, false, memory_order_release);
  for (int i = 0; i < pool_ptr->worker_pool_sz; i++) {
    UfsrvSchedulerStop(pool_ptr->workers[i]);
  }
}

void
UfsrvSchedulerPoolJoin(UfsrvSchedulerPool *pool_ptr)
{
  if (pool_ptr == NULL) {
    return;
  }
  for (int i = 0; i < pool_ptr->worker_pool_sz; i++) {
    UfsrvSchedulerJoin(pool_ptr->workers[i]);
  }
}

void
UfsrvSchedulerPoolDestroy(UfsrvSchedulerPool *pool_ptr)
{
  if (pool_ptr == NULL) {
    return;
  }

  if (atomic_load_explicit(&pool_ptr->is_running, memory_order_acquire)) {
    UfsrvSchedulerPoolStop(pool_ptr);
    UfsrvSchedulerPoolJoin(pool_ptr);
  }

  for (int i = 0; i < pool_ptr->worker_pool_sz; i++) {
    UfsrvSchedulerDestroy(pool_ptr->workers[i]);
  }

  free(pool_ptr->workers);
  free(pool_ptr);
}

int
UfsrvSchedulerPoolSize(const UfsrvSchedulerPool *pool_ptr)
{
  return pool_ptr != NULL ? pool_ptr->worker_pool_sz : 0;
}

/* ── Describe (JSON, no json-c) ──────────────────────────────────────────── */

/*!
 * @brief Append @p str to the BufferDescriptor as a JSON string literal,
 *        escaping the characters that would otherwise break the document.
 *
 * @param[in,out] bd   BufferDescriptor to append to.
 * @param[in]     str  String to emit (NULL → the JSON literal "null").
 */
static void
sDescribeJsonString(BufferDescriptor *bd, const char *str)
{
  if (str == NULL) {
    BufferDescriptorAppendFormatted(bd, "null");
    return;
  }
  BufferDescriptorAppendFormatted(bd, "\"");
  for (const char *p = str; *p != '\0'; p++) {
    switch (*p) {
    case '"':  BufferDescriptorAppendFormatted(bd, "\\\""); break;
    case '\\': BufferDescriptorAppendFormatted(bd, "\\\\"); break;
    case '\n': BufferDescriptorAppendFormatted(bd, "\\n");  break;
    case '\t': BufferDescriptorAppendFormatted(bd, "\\t");  break;
    default:   BufferDescriptorAppendFormatted(bd, "%c", *p); break;
    }
  }
  BufferDescriptorAppendFormatted(bd, "\"");
}

/*!
 * @brief Emit one worker as a JSON object (no leading comma; caller controls
 *        the array separators).
 *
 * @param[in,out] bd             BufferDescriptor to append to.
 * @param[in]     scheduler_ptr  Worker to describe.
 */
static void
sDescribeWorker(BufferDescriptor *bd, const UfsrvScheduler *scheduler_ptr)
{
  bool running  = atomic_load_explicit(&scheduler_ptr->is_running, memory_order_acquire);
  bool started  = atomic_load_explicit(&scheduler_ptr->is_started, memory_order_acquire);
  bool joined   = atomic_load_explicit(&scheduler_ptr->is_joined, memory_order_acquire);
  bool id_ready = atomic_load_explicit(&scheduler_ptr->is_worker_id_ready, memory_order_acquire);

  BufferDescriptorAppendFormatted(bd, "\n    {\"name\":");
  sDescribeJsonString(bd, scheduler_ptr->name);
  BufferDescriptorAppendFormatted(bd, ",\"is_running\":%s", running ? "true" : "false");
  BufferDescriptorAppendFormatted(bd, ",\"is_started\":%s", started ? "true" : "false");
  BufferDescriptorAppendFormatted(bd, ",\"is_joined\":%s", joined ? "true" : "false");
  BufferDescriptorAppendFormatted(bd, ",\"is_worker_id_ready\":%s", id_ready ? "true" : "false");
  if (id_ready) {
    /* pthread_t is unsigned long on Linux; emit it as a plain number. */
    BufferDescriptorAppendFormatted(bd, ",\"worker_id\":%lu", (unsigned long)scheduler_ptr->worker_id);
  }
  else {
    BufferDescriptorAppendFormatted(bd, ",\"worker_id\":null");
  }
  BufferDescriptorAppendFormatted(bd, ",\"event_fd\":%d}", scheduler_ptr->event_fd);
}

PUBLIC_API BufferDescriptor *
DescribeScheduler(UfsrvSchedulerPool *pool_ptr, const char *worker_name, BufferDescriptor *provided)
{
  if (provided == NULL) {
    provided = calloc(1, sizeof(BufferDescriptor));
    if (provided == NULL) {
      return NULL;
    }
    BufferDescriptorInit(provided, 256);
  }

  if (pool_ptr == NULL) {
    BufferDescriptorAppendFormatted(provided, "{\"worker_pool_sz\":0,\"workers\":[]}\n");
    return provided;
  }

  int  worker_pool_sz = pool_ptr->worker_pool_sz;
  int  next           = atomic_load_explicit(&pool_ptr->next, memory_order_relaxed);
  bool pool_running   = atomic_load_explicit(&pool_ptr->is_running, memory_order_acquire);

  BufferDescriptorAppendFormatted(provided,
      "{\"worker_pool_sz\":%d,\"round_robin_next\":%d,\"pool_is_running\":%s,\"workers\":[",
      worker_pool_sz, next, pool_running ? "true" : "false");

  bool emitted = false;
  for (int i = 0; i < worker_pool_sz; i++) {
    UfsrvScheduler *worker = pool_ptr->workers[i];
    if (worker_name != NULL && (worker->name == NULL || strcmp(worker->name, worker_name) != 0)) {
      continue;
    }
    if (emitted) {
      BufferDescriptorAppendFormatted(provided, ",");
    }
    sDescribeWorker(provided, worker);
    emitted = true;
  }

  BufferDescriptorAppendFormatted(provided, "\n]}\n");
  return provided;
}

PUBLIC_API CollectionDescriptor *
ListSchedulerWorkers(UfsrvSchedulerPool *pool_ptr)
{
  if (pool_ptr == NULL) {
    return NULL;
  }

  size_t n          = 0;
  size_t str_bytes  = 0;
  int    worker_pool_sz = pool_ptr->worker_pool_sz;

  /* Pass 1 — count named workers and total name bytes. */
  for (int i = 0; i < worker_pool_sz; i++) {
    UfsrvScheduler *worker = pool_ptr->workers[i];
    if (worker == NULL || worker->name == NULL) {
      continue;
    }
    n++;
    str_bytes += strlen(worker->name) + 1;
  }

  /* One contiguous slab: struct + pointer array + name strings. */
  size_t arr_bytes = n * sizeof(collection_t *);
  size_t total     = sizeof(CollectionDescriptor) + arr_bytes + str_bytes;
  char *block      = malloc(total);
  if (block == NULL) {
    return NULL;
  }

  CollectionDescriptor *cd = (CollectionDescriptor *)block;
  cd->collection           = (n > 0) ? (collection_t **)(block + sizeof(CollectionDescriptor)) : NULL;
  cd->collection_sz        = n;
  cd->collection_base_offset = 0;

  /* Pass 2 — copy the worker names into the same slab. */
  char *str = block + sizeof(CollectionDescriptor) + arr_bytes;
  size_t i  = 0;
  for (int t = 0; t < worker_pool_sz && i < n; t++) {
    UfsrvScheduler *worker = pool_ptr->workers[t];
    if (worker == NULL || worker->name == NULL) {
      continue;
    }
    size_t len = strlen(worker->name) + 1;
    memcpy(str, worker->name, len);
    cd->collection[i++] = (collection_t *)str;
    str += len;
  }

  return cd;
}
