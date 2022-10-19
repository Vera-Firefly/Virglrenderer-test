/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_RENDERER_H
#define VKR_RENDERER_H

#include "config.h"

#include <stddef.h>
#include <stdint.h>

#include "virgl_resource.h"
#include "virgl_util.h"

#define VKR_RENDERER_THREAD_SYNC (1u << 0)
#define VKR_RENDERER_ASYNC_FENCE_CB (1u << 1)

struct virgl_context;

#ifdef ENABLE_VENUS

int
vkr_renderer_init(uint32_t flags);

void
vkr_renderer_fini(void);

void
vkr_renderer_reset(void);

size_t
vkr_get_capset(void *capset);

struct virgl_context *
vkr_context_create(size_t debug_len, const char *debug_name);

bool
vkr_renderer_init2(uint32_t flags,
                   virgl_debug_callback_type debug_cb,
                   const struct virgl_renderer_callbacks *cbs);

void
vkr_renderer_fini2(void);

bool
vkr_renderer_create_context(uint32_t ctx_id,
                            uint32_t ctx_flags,
                            uint32_t nlen,
                            const char *name);

void
vkr_renderer_destroy_context(uint32_t ctx_id);

bool
vkr_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size);

bool
vkr_renderer_submit_fence(uint32_t ctx_id,
                          uint32_t flags,
                          uint64_t ring_idx,
                          uint64_t fence_id);

bool
vkr_renderer_create_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             uint32_t blob_flags,
                             uint64_t blob_id,
                             uint64_t blob_size,
                             enum virgl_resource_fd_type *out_fd_type,
                             int *out_res_fd,
                             uint32_t *out_map_info,
                             struct virgl_resource_vulkan_info *out_vulkan_info);

bool
vkr_renderer_import_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             enum virgl_resource_fd_type fd_type,
                             int fd,
                             uint64_t size);

void
vkr_renderer_destroy_resource(uint32_t ctx_id, uint32_t res_id);

#else /* ENABLE_VENUS */

#include <stdio.h>

static inline int
vkr_renderer_init(UNUSED uint32_t flags)
{
   virgl_log("Vulkan support was not enabled in virglrenderer\n");
   return -1;
}

static inline void
vkr_renderer_fini(void)
{
}

static inline void
vkr_renderer_reset(void)
{
}

static inline size_t
vkr_get_capset(UNUSED void *capset)
{
   return 0;
}

static inline struct virgl_context *
vkr_context_create(UNUSED size_t debug_len, UNUSED const char *debug_name)
{
   return NULL;
}

static inline bool
vkr_renderer_init2(UNUSED uint32_t flags,
                   UNUSED virgl_debug_callback_type debug_cb,
                   UNUSED const struct virgl_renderer_callbacks *cbs)
{
   virgl_log("Vulkan support was not enabled in virglrenderer\n");
   return false;
}

static inline void
vkr_renderer_fini2(void)
{
   return;
}

static inline bool
vkr_renderer_create_context(UNUSED uint32_t ctx_id,
                            UNUSED uint32_t ctx_flags,
                            UNUSED uint32_t nlen,
                            UNUSED const char *name)
{
   return false;
}

static inline void
vkr_renderer_destroy_context(UNUSED uint32_t ctx_id)
{
   return;
}

static inline bool
vkr_renderer_submit_cmd(UNUSED uint32_t ctx_id, UNUSED void *cmd, UNUSED uint32_t size)
{
   return false;
}

static inline bool
vkr_renderer_submit_fence(UNUSED uint32_t ctx_id,
                          UNUSED uint32_t flags,
                          UNUSED uint64_t ring_idx,
                          UNUSED uint64_t fence_id)
{
   return false;
}

static inline bool
vkr_renderer_create_resource(UNUSED uint32_t ctx_id,
                             UNUSED uint32_t res_id,
                             UNUSED uint32_t blob_flags,
                             UNUSED uint64_t blob_id,
                             UNUSED uint64_t blob_size,
                             UNUSED enum virgl_resource_fd_type *out_fd_type,
                             UNUSED int *out_res_fd,
                             UNUSED uint32_t *out_map_info,
                             UNUSED struct virgl_resource_vulkan_info *out_vulkan_info)
{
   return false;
}

static inline bool
vkr_renderer_import_resource(UNUSED uint32_t ctx_id,
                             UNUSED uint32_t res_id,
                             UNUSED enum virgl_resource_fd_type fd_type,
                             UNUSED int fd,
                             UNUSED uint64_t size)
{
   return false;
}

static inline void
vkr_renderer_destroy_resource(UNUSED uint32_t ctx_id, UNUSED uint32_t res_id)
{
   return;
}

#endif /* ENABLE_VENUS */

#endif /* VKR_RENDERER_H */
