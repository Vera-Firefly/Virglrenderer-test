/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_PHYSICAL_DEVICE_H
#define VKR_PHYSICAL_DEVICE_H

#include "vkr_common.h"

struct vkr_physical_device {
   struct vkr_object base;

   VkPhysicalDeviceProperties properties;
   uint32_t api_version;

   VkExtensionProperties *extensions;
   uint32_t extension_count;

   bool KHR_external_memory_fd;
   bool EXT_external_memory_dma_buf;

   bool KHR_external_fence_fd;

   VkPhysicalDeviceMemoryProperties memory_properties;

   struct list_head devices;
};

void
vkr_context_init_physical_device_dispatch(struct vkr_context *ctx);

#endif /* VKR_PHYSICAL_DEVICE_H */
