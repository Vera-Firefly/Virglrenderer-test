/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_BUFFER_H
#define VKR_BUFFER_H

#include "vkr_common.h"

struct vkr_buffer {
   struct vkr_object base;
};

struct vkr_buffer_view {
   struct vkr_object base;
};

void
vkr_context_init_buffer_dispatch(struct vkr_context *ctx);

void
vkr_context_init_buffer_view_dispatch(struct vkr_context *ctx);

#endif /* VKR_BUFFER_H */
