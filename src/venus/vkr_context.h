/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_CONTEXT_H
#define VKR_CONTEXT_H

#include "vkr_common.h"

#include "venus-protocol/vn_protocol_renderer_defines.h"
#include "virgl_context.h"

#include "vkr_cs.h"

struct virgl_resource;

/*
 * When a virgl_resource is attached in vkr_context_attach_resource, a
 * vkr_resource_attachment is created.  A vkr_resource_attachment is valid
 * until the resource it tracks is detached.
 *
 * To support transfers to resources not backed by coherent dma-bufs, we
 * associate a vkr_resource_attachment with a (list of) vkr_device_memory.
 * This way, we can find a vkr_device_memory from a vkr_resource_attachment
 * and do transfers using VkDeviceMemory.
 */
struct vkr_resource_attachment {
   struct virgl_resource *resource;
   struct list_head memories;
};

enum vkr_context_validate_level {
   /* no validation */
   VKR_CONTEXT_VALIDATE_NONE,
   /* force enabling a subset of the validation layer */
   VKR_CONTEXT_VALIDATE_ON,
   /* force enabling the validation layer */
   VKR_CONTEXT_VALIDATE_FULL,
};

struct vkr_context {
   struct virgl_context base;

   char *debug_name;
   enum vkr_context_validate_level validate_level;
   bool validate_fatal;

   mtx_t mutex;

   struct list_head rings;
   struct util_hash_table_u64 *object_table;
   struct util_hash_table *resource_table;
   struct list_head newly_exported_memories;

   struct vkr_cs_encoder encoder;
   struct vkr_cs_decoder decoder;
   struct vn_dispatch_context dispatch;

   int fence_eventfd;
   struct list_head busy_queues;

   struct vkr_instance *instance;
};

#endif /* VKR_CONTEXT_H */
