/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_RING_H
#define VKR_RING_H

#include "vkr_common.h"

/* The layout of a ring in a virgl_resource.  This is parsed and discarded by
 * vkr_ring_create.
 */
struct vkr_ring_layout {
   struct virgl_resource *resource;

   struct vkr_region head;
   struct vkr_region tail;
   struct vkr_region status;
   struct vkr_region buffer;
   struct vkr_region extra;
};

static_assert(ATOMIC_INT_LOCK_FREE == 2 && sizeof(atomic_uint) == 4,
              "vkr_ring_control requires lock-free 32-bit atomic_uint");

/* the control region of a ring */
struct vkr_ring_control {
   /* Pointers to ring head, tail, and status.
    *
    * Clients increment the tail after commands are added.  We increment the
    * head after commands are executed.  The status is updated when there is a
    * status change to the ring thread.
    */
   volatile atomic_uint *head;
   const volatile atomic_uint *tail;
   volatile atomic_uint *status;
};

struct vkr_ring_shared {
   const void *buffer;

   void *extra;
};

struct vkr_ring {
   /* used by the caller */
   vkr_object_id id;
   struct list_head head;

   /* ring regions */
   struct virgl_resource *resource;
   struct vkr_ring_control control;

   struct vkr_ring_shared shared;
   uint32_t buffer_size;
   uint32_t buffer_mask;
   uint32_t cur;
   void *cmd;

   size_t extra_size;

   /* ring thread */
   struct virgl_context *context;
   uint64_t idle_timeout;

   mtx_t mutex;
   cnd_t cond;
   thrd_t thread;
   atomic_bool started;
   atomic_bool pending_notify;
};

struct vkr_ring *
vkr_ring_create(const struct vkr_ring_layout *layout,
                struct virgl_context *ctx,
                uint64_t idle_timeout);

void
vkr_ring_destroy(struct vkr_ring *ring);

void
vkr_ring_start(struct vkr_ring *ring);

bool
vkr_ring_stop(struct vkr_ring *ring);

void
vkr_ring_notify(struct vkr_ring *ring);

bool
vkr_ring_write_extra(struct vkr_ring *ring, size_t offset, uint32_t val);

#endif /* VKR_RING_H */
