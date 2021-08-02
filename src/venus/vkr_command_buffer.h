/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_COMMAND_BUFFER_H
#define VKR_COMMAND_BUFFER_H

#include "vkr_common.h"

struct vkr_command_pool {
   struct vkr_object base;

   struct list_head command_buffers;
};

struct vkr_command_buffer {
   struct vkr_object base;

   struct vkr_device *device;
};

void
vkr_context_init_command_pool_dispatch(struct vkr_context *ctx);

void
vkr_context_init_command_buffer_dispatch(struct vkr_context *ctx);

#endif /* VKR_COMMAND_BUFFER_H */
