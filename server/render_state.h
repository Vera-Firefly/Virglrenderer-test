/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef RENDER_STATE_H
#define RENDER_STATE_H

#include "render_common.h"

#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
#include "c11/threads.h"
#endif

/* Workers call into virglrenderer.  When they are processes, not much care is
 * required. But when workers are threads, we need to grab a lock to protect
 * virglrenderer.
 *
 * TODO skip virglrenderer.h and go straight to vkr_renderer.h.
 */
struct render_state {
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   /* protect the struct */
   mtx_t struct_mutex;
   /* protect virglrenderer interface */
   mtx_t dispatch_mutex;
#endif

   /* track and init/fini just once */
   int init_count;
   uint32_t init_flags;

   /* track the render_context */
   struct list_head contexts;
};

extern struct render_state render_state_internal;

bool
render_state_init(uint32_t init_flags);

void
render_state_fini(void);

void
render_state_add_context(struct render_context *ctx);

void
render_state_remove_context(struct render_context *ctx);

static inline void
render_state_lock_dispatch(void)
{
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   mtx_lock(&render_state_internal.dispatch_mutex);
#endif
}

static inline void
render_state_unlock_dispatch(void)
{
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   mtx_unlock(&render_state_internal.dispatch_mutex);
#endif
}

bool
render_state_create_context(uint32_t ctx_id,
                            uint32_t flags,
                            uint32_t name_len,
                            const char *name);

void
render_state_destroy_context(uint32_t ctx_id);

bool
render_state_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size);

bool
render_state_submit_fence(uint32_t ctx_id,
                          uint32_t flags,
                          uint64_t queue_id,
                          uint64_t fence_id);

bool
render_state_create_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             uint32_t blob_flags,
                             uint64_t blob_id,
                             uint64_t blob_size,
                             enum virgl_resource_fd_type *out_fd_type,
                             int *out_res_fd,
                             uint32_t *out_map_info);

bool
render_state_import_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             enum virgl_resource_fd_type fd_type,
                             int fd,
                             uint64_t size);

void
render_state_destroy_resource(uint32_t ctx_id, uint32_t res_id);

#endif /* RENDER_STATE_H */
