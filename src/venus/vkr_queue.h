/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_QUEUE_H
#define VKR_QUEUE_H

#include "vkr_common.h"

struct vkr_queue_sync {
   VkFence fence;

   uint32_t flags;
   void *fence_cookie;

   struct list_head head;
};

struct vkr_queue {
   struct vkr_object base;

   struct vkr_context *context;
   struct vkr_device *device;

   uint32_t family;
   uint32_t index;

   /* Submitted fences are added to pending_syncs first.  How submitted fences
    * are retired depends on VKR_RENDERER_THREAD_SYNC and
    * VKR_RENDERER_ASYNC_FENCE_CB.
    *
    * When VKR_RENDERER_THREAD_SYNC is not set, the main thread calls
    * vkGetFenceStatus and retires signaled fences in pending_syncs in order.
    *
    * When VKR_RENDERER_THREAD_SYNC is set but VKR_RENDERER_ASYNC_FENCE_CB is
    * not set, the sync thread calls vkWaitForFences and moves signaled fences
    * from pending_syncs to signaled_syncs in order.  The main thread simply
    * retires all fences in signaled_syncs.
    *
    * When VKR_RENDERER_THREAD_SYNC and VKR_RENDERER_ASYNC_FENCE_CB are both
    * set, the sync thread calls vkWaitForFences and retires signaled fences
    * in pending_syncs in order.
    */
   int eventfd;
   thrd_t thread;
   mtx_t mutex;
   cnd_t cond;
   bool join;
   struct list_head pending_syncs;
   struct list_head signaled_syncs;

   struct list_head head;
   struct list_head busy_head;
};

struct vkr_fence {
   struct vkr_object base;
};

struct vkr_semaphore {
   struct vkr_object base;
};

struct vkr_event {
   struct vkr_object base;
};

#endif /* VKR_QUEUE_H */
