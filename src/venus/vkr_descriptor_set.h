/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DESCRIPTOR_SET_H
#define VKR_DESCRIPTOR_SET_H

#include "vkr_common.h"

struct vkr_descriptor_set_layout {
   struct vkr_object base;
};

struct vkr_descriptor_pool {
   struct vkr_object base;

   struct list_head descriptor_sets;
};

struct vkr_descriptor_set {
   struct vkr_object base;

   struct vkr_device *device;

   struct list_head head;
};

struct vkr_descriptor_update_template {
   struct vkr_object base;
};

void
vkr_context_init_descriptor_set_layout_dispatch(struct vkr_context *ctx);

void
vkr_context_init_descriptor_pool_dispatch(struct vkr_context *ctx);

void
vkr_context_init_descriptor_set_dispatch(struct vkr_context *ctx);

void
vkr_context_init_descriptor_update_template_dispatch(struct vkr_context *ctx);

#endif /* VKR_DESCRIPTOR_SET_H */
