/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_PIPELINE_H
#define VKR_PIPELINE_H

#include "vkr_common.h"

struct vkr_shader_module {
   struct vkr_object base;
};

struct vkr_pipeline_layout {
   struct vkr_object base;
};

struct vkr_pipeline_cache {
   struct vkr_object base;
};

struct vkr_pipeline {
   struct vkr_object base;
};

void
vkr_context_init_shader_module_dispatch(struct vkr_context *ctx);

void
vkr_context_init_pipeline_layout_dispatch(struct vkr_context *ctx);

void
vkr_context_init_pipeline_cache_dispatch(struct vkr_context *ctx);

void
vkr_context_init_pipeline_dispatch(struct vkr_context *ctx);

#endif /* VKR_PIPELINE_H */
