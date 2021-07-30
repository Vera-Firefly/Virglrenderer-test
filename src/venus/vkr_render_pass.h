/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_RENDER_PASS_H
#define VKR_RENDER_PASS_H

#include "vkr_common.h"

struct vkr_render_pass {
   struct vkr_object base;
};

struct vkr_framebuffer {
   struct vkr_object base;
};

void
vkr_context_init_render_pass_dispatch(struct vkr_context *ctx);

void
vkr_context_init_framebuffer_dispatch(struct vkr_context *ctx);

#endif /* VKR_RENDER_PASS_H */
