/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_MEMORY_H
#define VKR_DEVICE_MEMORY_H

#include "vkr_common.h"

struct gbm_bo;

struct vkr_device_memory {
   struct vkr_object base;

   struct vkr_device *device;
   uint32_t property_flags;
   uint32_t valid_fd_types;

   /* gbm bo backing non-external mappable memory */
   struct gbm_bo *gbm_bo;

   bool exported;
   uint32_t exported_res_id;
   struct list_head exported_head;
};
VKR_DEFINE_OBJECT_CAST(device_memory, VK_OBJECT_TYPE_DEVICE_MEMORY, VkDeviceMemory)

void
vkr_context_init_device_memory_dispatch(struct vkr_context *ctx);

void
vkr_device_memory_release(struct vkr_device_memory *mem);

int
vkr_device_memory_export_fd(struct vkr_device_memory *mem,
                            VkExternalMemoryHandleTypeFlagBits handle_type,
                            int *out_fd);

#endif /* VKR_DEVICE_MEMORY_H */
