/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef RENDER_STATE_H
#define RENDER_STATE_H

#include "render_common.h"

bool
render_state_init(uint32_t init_flags);

void
render_state_fini(void);

void
render_state_add_context(struct render_context *ctx);

void
render_state_remove_context(struct render_context *ctx);

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
