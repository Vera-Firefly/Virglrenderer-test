/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_DEVICE_MEMORY_H
#define VKR_DEVICE_MEMORY_H

#include "vkr_common.h"

struct vkr_device_memory {
   struct vkr_object base;

   VkDevice device;
   uint32_t property_flags;
   uint32_t valid_fd_types;

   bool exported;
   uint32_t exported_res_id;
   struct list_head head;
};

#endif /* VKR_DEVICE_MEMORY_H */
