/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_common.h"

#include <stdio.h>

#include "pipe/p_state.h"
#include "util/u_debug.h"
#include "venus-protocol/vn_protocol_renderer.h"
#include "virgl_context.h"
#include "virgl_protocol.h" /* for transfer_mode */
#include "virgl_resource.h"
#include "virglrenderer.h"
#include "virglrenderer_hw.h"
#include "vrend_debug.h"
#include "vrend_iov.h"

#include "vkr_buffer.h"
#include "vkr_command_buffer.h"
#include "vkr_context.h"
#include "vkr_cs.h"
#include "vkr_descriptor_set.h"
#include "vkr_device.h"
#include "vkr_device_memory.h"
#include "vkr_image.h"
#include "vkr_pipeline.h"
#include "vkr_query_pool.h"
#include "vkr_queue.h"
#include "vkr_render_pass.h"
#include "vkr_ring.h"

static const struct debug_named_value vkr_debug_options[] = {
   { "validate", VKR_DEBUG_VALIDATE, "Force enabling the validation layer" },
   DEBUG_NAMED_VALUE_END
};

uint32_t vkr_renderer_flags;
uint32_t vkr_debug_flags;

static void
vkr_dispatch_vkSetReplyCommandStreamMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkSetReplyCommandStreamMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_resource_attachment *att;

   if (!args->pStream) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   att = util_hash_table_get(ctx->resource_table,
                             uintptr_to_pointer(args->pStream->resourceId));
   if (!att) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vkr_cs_encoder_set_stream(&ctx->encoder, att->resource->iov, att->resource->iov_count,
                             args->pStream->offset, args->pStream->size);
}

static void
vkr_dispatch_vkSeekReplyCommandStreamMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkSeekReplyCommandStreamMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   vkr_cs_encoder_seek_stream(&ctx->encoder, args->position);
}

static void *
copy_command_stream(struct vkr_context *ctx, const VkCommandStreamDescriptionMESA *stream)
{
   struct vkr_resource_attachment *att;
   struct virgl_resource *res;

   att = util_hash_table_get(ctx->resource_table, uintptr_to_pointer(stream->resourceId));
   if (!att)
      return NULL;
   res = att->resource;

   /* seek to offset */
   size_t iov_offset = stream->offset;
   const struct iovec *iov = NULL;
   for (int i = 0; i < res->iov_count; i++) {
      if (iov_offset < res->iov[i].iov_len) {
         iov = &res->iov[i];
         break;
      }
      iov_offset -= res->iov[i].iov_len;
   }
   if (!iov)
      return NULL;

   /* XXX until the decoder supports scatter-gather and is robust enough,
    * always make a copy in case the caller modifies the commands while we
    * parse
    */
   uint8_t *data = malloc(stream->size);
   if (!data)
      return NULL;

   uint32_t copied = 0;
   while (true) {
      const size_t s = MIN2(stream->size - copied, iov->iov_len - iov_offset);
      memcpy(data + copied, (const uint8_t *)iov->iov_base + iov_offset, s);

      copied += s;
      if (copied == stream->size) {
         break;
      } else if (iov == &res->iov[res->iov_count - 1]) {
         free(data);
         return NULL;
      }

      iov++;
      iov_offset = 0;
   }

   return data;
}

static void
vkr_dispatch_vkExecuteCommandStreamsMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkExecuteCommandStreamsMESA *args)
{
   struct vkr_context *ctx = dispatch->data;

   if (!args->streamCount || !args->pStreams) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   /* note that nested vkExecuteCommandStreamsMESA is not allowed */
   if (!vkr_cs_decoder_push_state(&ctx->decoder)) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   for (uint32_t i = 0; i < args->streamCount; i++) {
      const VkCommandStreamDescriptionMESA *stream = &args->pStreams[i];

      if (args->pReplyPositions)
         vkr_cs_encoder_seek_stream(&ctx->encoder, args->pReplyPositions[i]);

      if (!stream->size)
         continue;

      void *data = copy_command_stream(ctx, stream);
      if (!data) {
         vkr_cs_decoder_set_fatal(&ctx->decoder);
         break;
      }

      vkr_cs_decoder_set_stream(&ctx->decoder, data, stream->size);
      while (vkr_cs_decoder_has_command(&ctx->decoder)) {
         vn_dispatch_command(&ctx->dispatch);
         if (vkr_cs_decoder_get_fatal(&ctx->decoder))
            break;
      }

      free(data);

      if (vkr_cs_decoder_get_fatal(&ctx->decoder))
         break;
   }

   vkr_cs_decoder_pop_state(&ctx->decoder);
}

static struct vkr_ring *
lookup_ring(struct vkr_context *ctx, uint64_t ring_id)
{
   struct vkr_ring *ring;
   LIST_FOR_EACH_ENTRY (ring, &ctx->rings, head) {
      if (ring->id == ring_id)
         return ring;
   }
   return NULL;
}

static void
vkr_dispatch_vkCreateRingMESA(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCreateRingMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   const VkRingCreateInfoMESA *info = args->pCreateInfo;
   const struct vkr_resource_attachment *att;
   uint8_t *shared;
   size_t size;
   struct vkr_ring *ring;

   if (!info) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   att = util_hash_table_get(ctx->resource_table, uintptr_to_pointer(info->resourceId));
   if (!att) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   /* TODO support scatter-gather or require logically contiguous resources */
   if (att->resource->iov_count != 1) {
      vrend_printf("vkr: no scatter-gather support for ring buffers (TODO)");
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   shared = att->resource->iov[0].iov_base;
   size = att->resource->iov[0].iov_len;
   if (info->offset > size || info->size > size - info->offset) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   shared += info->offset;
   size = info->size;
   if (info->headOffset > size || info->tailOffset > size || info->statusOffset > size ||
       info->bufferOffset > size || info->extraOffset > size) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }
   if (sizeof(uint32_t) > size - info->headOffset ||
       sizeof(uint32_t) > size - info->tailOffset ||
       sizeof(uint32_t) > size - info->statusOffset ||
       info->bufferSize > size - info->bufferOffset ||
       info->extraSize > size - info->extraOffset) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }
   if (!info->bufferSize || !util_is_power_of_two(info->bufferSize)) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   const struct vkr_ring_layout layout = {
      .head_offset = info->headOffset,
      .tail_offset = info->tailOffset,
      .status_offset = info->statusOffset,
      .buffer_offset = info->bufferOffset,
      .buffer_size = info->bufferSize,
      .extra_offset = info->extraOffset,
      .extra_size = info->extraSize,
   };

   ring = vkr_ring_create(&layout, shared, &ctx->base, info->idleTimeout);
   if (!ring) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   ring->id = args->ring;
   list_addtail(&ring->head, &ctx->rings);

   vkr_ring_start(ring);
}

static void
vkr_dispatch_vkDestroyRingMESA(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkDestroyRingMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_ring *ring = lookup_ring(ctx, args->ring);
   if (!ring || !vkr_ring_stop(ring)) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   list_del(&ring->head);
   vkr_ring_destroy(ring);
}

static void
vkr_dispatch_vkNotifyRingMESA(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkNotifyRingMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_ring *ring = lookup_ring(ctx, args->ring);
   if (!ring) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vkr_ring_notify(ring);
}

static void
vkr_dispatch_vkWriteRingExtraMESA(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkWriteRingExtraMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_ring *ring = lookup_ring(ctx, args->ring);
   if (!ring) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   if (!vkr_ring_write_extra(ring, args->offset, args->value))
      vkr_cs_decoder_set_fatal(&ctx->decoder);
}

static void
vkr_dispatch_vkGetVenusExperimentalFeatureData100000MESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetVenusExperimentalFeatureData100000MESA *args)
{
   struct vkr_context *ctx = dispatch->data;

   if (!args->pDataSize) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   const VkVenusExperimentalFeatures100000MESA features = {
      .memoryResourceAllocationSize = VK_TRUE,
   };

   vn_replace_vkGetVenusExperimentalFeatureData100000MESA_args_handle(args);

   if (!args->pData) {
      *args->pDataSize = sizeof(features);
      return;
   }

   *args->pDataSize = MIN2(*args->pDataSize, sizeof(features));
   memcpy(args->pData, &features, *args->pDataSize);
}

static void
vkr_dispatch_debug_log(UNUSED struct vn_dispatch_context *dispatch, const char *msg)
{
   vrend_printf("vkr: %s\n", msg);
}

static void
vkr_context_init_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->data = ctx;
   dispatch->debug_log = vkr_dispatch_debug_log;

   dispatch->encoder = (struct vn_cs_encoder *)&ctx->encoder;
   dispatch->decoder = (struct vn_cs_decoder *)&ctx->decoder;

   dispatch->dispatch_vkSetReplyCommandStreamMESA =
      vkr_dispatch_vkSetReplyCommandStreamMESA;
   dispatch->dispatch_vkSeekReplyCommandStreamMESA =
      vkr_dispatch_vkSeekReplyCommandStreamMESA;
   dispatch->dispatch_vkExecuteCommandStreamsMESA =
      vkr_dispatch_vkExecuteCommandStreamsMESA;
   dispatch->dispatch_vkCreateRingMESA = vkr_dispatch_vkCreateRingMESA;
   dispatch->dispatch_vkDestroyRingMESA = vkr_dispatch_vkDestroyRingMESA;
   dispatch->dispatch_vkNotifyRingMESA = vkr_dispatch_vkNotifyRingMESA;
   dispatch->dispatch_vkWriteRingExtraMESA = vkr_dispatch_vkWriteRingExtraMESA;

   vkr_context_init_instance_dispatch(ctx);
   vkr_context_init_physical_device_dispatch(ctx);
   vkr_context_init_device_dispatch(ctx);

   vkr_context_init_queue_dispatch(ctx);
   vkr_context_init_fence_dispatch(ctx);
   vkr_context_init_semaphore_dispatch(ctx);
   vkr_context_init_event_dispatch(ctx);

   vkr_context_init_device_memory_dispatch(ctx);

   vkr_context_init_buffer_dispatch(ctx);
   vkr_context_init_buffer_view_dispatch(ctx);

   vkr_context_init_image_dispatch(ctx);
   vkr_context_init_image_view_dispatch(ctx);
   vkr_context_init_sampler_dispatch(ctx);
   vkr_context_init_sampler_ycbcr_conversion_dispatch(ctx);

   vkr_context_init_descriptor_set_layout_dispatch(ctx);
   vkr_context_init_descriptor_pool_dispatch(ctx);
   vkr_context_init_descriptor_set_dispatch(ctx);
   vkr_context_init_descriptor_update_template_dispatch(ctx);

   vkr_context_init_render_pass_dispatch(ctx);
   vkr_context_init_framebuffer_dispatch(ctx);

   vkr_context_init_query_pool_dispatch(ctx);

   vkr_context_init_shader_module_dispatch(ctx);
   vkr_context_init_pipeline_layout_dispatch(ctx);
   vkr_context_init_pipeline_cache_dispatch(ctx);
   vkr_context_init_pipeline_dispatch(ctx);

   vkr_context_init_command_pool_dispatch(ctx);
   vkr_context_init_command_buffer_dispatch(ctx);

   dispatch->dispatch_vkGetVenusExperimentalFeatureData100000MESA =
      vkr_dispatch_vkGetVenusExperimentalFeatureData100000MESA;
}

static int
vkr_context_submit_fence_locked(struct virgl_context *base,
                                uint32_t flags,
                                uint64_t queue_id,
                                void *fence_cookie)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   struct vkr_queue *queue;
   VkResult result;

   queue = util_hash_table_get_u64(ctx->object_table, queue_id);
   if (!queue)
      return -EINVAL;
   struct vkr_device *dev = queue->device;

   struct vkr_queue_sync *sync = vkr_device_alloc_queue_sync(dev, flags, fence_cookie);
   if (!sync)
      return -ENOMEM;

   result = vkQueueSubmit(queue->base.handle.queue, 0, NULL, sync->fence);
   if (result != VK_SUCCESS) {
      vkr_device_free_queue_sync(dev, sync);
      return -1;
   }

   if (vkr_renderer_flags & VKR_RENDERER_THREAD_SYNC) {
      mtx_lock(&queue->mutex);
      list_addtail(&sync->head, &queue->pending_syncs);
      mtx_unlock(&queue->mutex);
      cnd_signal(&queue->cond);
   } else {
      list_addtail(&sync->head, &queue->pending_syncs);
   }

   if (LIST_IS_EMPTY(&queue->busy_head))
      list_addtail(&queue->busy_head, &ctx->busy_queues);

   return 0;
}

static int
vkr_context_submit_fence(struct virgl_context *base,
                         uint32_t flags,
                         uint64_t queue_id,
                         void *fence_cookie)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   int ret;

   mtx_lock(&ctx->mutex);
   ret = vkr_context_submit_fence_locked(base, flags, queue_id, fence_cookie);
   mtx_unlock(&ctx->mutex);
   return ret;
}

static void
vkr_context_retire_fences_locked(UNUSED struct virgl_context *base)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   struct vkr_queue_sync *sync, *sync_tmp;
   struct vkr_queue *queue, *queue_tmp;

   assert(!(vkr_renderer_flags & VKR_RENDERER_ASYNC_FENCE_CB));

   /* flush first and once because the per-queue sync threads might write to
    * it any time
    */
   if (ctx->fence_eventfd >= 0)
      flush_eventfd(ctx->fence_eventfd);

   LIST_FOR_EACH_ENTRY_SAFE (queue, queue_tmp, &ctx->busy_queues, busy_head) {
      struct vkr_device *dev = queue->device;
      struct list_head retired_syncs;
      bool queue_empty;

      vkr_queue_retire_syncs(queue, &retired_syncs, &queue_empty);

      LIST_FOR_EACH_ENTRY_SAFE (sync, sync_tmp, &retired_syncs, head) {
         ctx->base.fence_retire(&ctx->base, queue->base.id, sync->fence_cookie);
         vkr_device_free_queue_sync(dev, sync);
      }

      if (queue_empty)
         list_delinit(&queue->busy_head);
   }
}

static void
vkr_context_retire_fences(struct virgl_context *base)
{
   struct vkr_context *ctx = (struct vkr_context *)base;

   if (vkr_renderer_flags & VKR_RENDERER_ASYNC_FENCE_CB)
      return;

   mtx_lock(&ctx->mutex);
   vkr_context_retire_fences_locked(base);
   mtx_unlock(&ctx->mutex);
}

static int
vkr_context_get_fencing_fd(struct virgl_context *base)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   return ctx->fence_eventfd;
}

static int
vkr_context_submit_cmd(struct virgl_context *base, const void *buffer, size_t size)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   int ret = 0;

   mtx_lock(&ctx->mutex);

   /* CS error is considered fatal (destroy the context?) */
   if (vkr_cs_decoder_get_fatal(&ctx->decoder))
      return EINVAL;

   vkr_cs_decoder_set_stream(&ctx->decoder, buffer, size);

   while (vkr_cs_decoder_has_command(&ctx->decoder)) {
      vn_dispatch_command(&ctx->dispatch);
      if (vkr_cs_decoder_get_fatal(&ctx->decoder)) {
         ret = EINVAL;
         break;
      }
   }

   vkr_cs_decoder_reset(&ctx->decoder);

   mtx_unlock(&ctx->mutex);

   return ret;
}

static int
vkr_context_get_blob_locked(struct virgl_context *base,
                            uint64_t blob_id,
                            uint32_t flags,
                            struct virgl_context_blob *blob)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   struct vkr_device_memory *mem;
   enum virgl_resource_fd_type fd_type = VIRGL_RESOURCE_FD_INVALID;

   mem = util_hash_table_get_u64(ctx->object_table, blob_id);
   if (!mem || mem->base.type != VK_OBJECT_TYPE_DEVICE_MEMORY)
      return EINVAL;

   /* a memory can only be exported once; we don't want two resources to point
    * to the same storage.
    */
   if (mem->exported)
      return EINVAL;

   if (!mem->valid_fd_types)
      return EINVAL;

   if (flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE) {
      const bool host_visible = mem->property_flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
      if (!host_visible)
         return EINVAL;
   }

   if (flags & VIRGL_RENDERER_BLOB_FLAG_USE_CROSS_DEVICE) {
      if (!(mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_DMABUF)))
         return EINVAL;

      fd_type = VIRGL_RESOURCE_FD_DMABUF;
   }

   if (fd_type == VIRGL_RESOURCE_FD_INVALID) {
      /* prefer dmabuf for easier mapping?  prefer opaque for performance? */
      if (mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_DMABUF))
         fd_type = VIRGL_RESOURCE_FD_DMABUF;
      else if (mem->valid_fd_types & (1 << VIRGL_RESOURCE_FD_OPAQUE))
         fd_type = VIRGL_RESOURCE_FD_OPAQUE;
   }

   int fd = -1;
   if (fd_type != VIRGL_RESOURCE_FD_INVALID) {
      VkExternalMemoryHandleTypeFlagBits handle_type;
      switch (fd_type) {
      case VIRGL_RESOURCE_FD_DMABUF:
         handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
         break;
      case VIRGL_RESOURCE_FD_OPAQUE:
         handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
         break;
      default:
         return EINVAL;
      }

      VkResult result = ctx->instance->get_memory_fd(
         mem->device,
         &(VkMemoryGetFdInfoKHR){
            .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
            .memory = mem->base.handle.device_memory,
            .handleType = handle_type,
         },
         &fd);
      if (result != VK_SUCCESS)
         return EINVAL;
   }

   blob->type = fd_type;
   blob->u.fd = fd;

   if (flags & VIRGL_RENDERER_BLOB_FLAG_USE_MAPPABLE) {
      const bool host_coherent =
         mem->property_flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      const bool host_cached = mem->property_flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

      /* XXX guessed */
      if (host_coherent) {
         blob->map_info =
            host_cached ? VIRGL_RENDERER_MAP_CACHE_CACHED : VIRGL_RENDERER_MAP_CACHE_WC;
      } else {
         blob->map_info = VIRGL_RENDERER_MAP_CACHE_WC;
      }
   } else {
      blob->map_info = VIRGL_RENDERER_MAP_CACHE_NONE;
   }

   blob->renderer_data = mem;

   return 0;
}

static int
vkr_context_get_blob(struct virgl_context *base,
                     uint64_t blob_id,
                     uint32_t flags,
                     struct virgl_context_blob *blob)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   int ret;

   mtx_lock(&ctx->mutex);
   ret = vkr_context_get_blob_locked(base, blob_id, flags, blob);
   /* XXX unlock in vkr_context_get_blob_done on success */
   if (ret)
      mtx_unlock(&ctx->mutex);

   return ret;
}

static void
vkr_context_get_blob_done(struct virgl_context *base,
                          uint32_t res_id,
                          struct virgl_context_blob *blob)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   struct vkr_device_memory *mem = blob->renderer_data;

   mem->exported = true;
   mem->exported_res_id = res_id;
   list_add(&mem->head, &ctx->newly_exported_memories);

   /* XXX locked in vkr_context_get_blob */
   mtx_unlock(&ctx->mutex);
}

static int
vkr_context_transfer_3d_locked(struct virgl_context *base,
                               struct virgl_resource *res,
                               const struct vrend_transfer_info *info,
                               int transfer_mode)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   struct vkr_resource_attachment *att;
   const struct iovec *iov;
   int iov_count;

   if (info->level || info->stride || info->layer_stride)
      return EINVAL;

   if (info->iovec) {
      iov = info->iovec;
      iov_count = info->iovec_cnt;
   } else {
      iov = res->iov;
      iov_count = res->iov_count;
   }

   if (!iov || !iov_count)
      return 0;

   att = util_hash_table_get(ctx->resource_table, uintptr_to_pointer(res->res_id));
   if (!att)
      return EINVAL;

   assert(att->resource == res);

   /* TODO transfer via dmabuf (and find a solution to coherency issues) */
   if (LIST_IS_EMPTY(&att->memories)) {
      vrend_printf("unable to transfer without VkDeviceMemory (TODO)");
      return EINVAL;
   }

   struct vkr_device_memory *mem =
      LIST_ENTRY(struct vkr_device_memory, att->memories.next, head);
   const VkMappedMemoryRange range = {
      .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
      .memory = mem->base.handle.device_memory,
      .offset = info->box->x,
      .size = info->box->width,
   };

   void *ptr;
   VkResult result =
      vkMapMemory(mem->device, range.memory, range.offset, range.size, 0, &ptr);
   if (result != VK_SUCCESS)
      return EINVAL;

   if (transfer_mode == VIRGL_TRANSFER_TO_HOST) {
      vrend_read_from_iovec(iov, iov_count, range.offset, ptr, range.size);
      vkFlushMappedMemoryRanges(mem->device, 1, &range);
   } else {
      vkInvalidateMappedMemoryRanges(mem->device, 1, &range);
      vrend_write_to_iovec(iov, iov_count, range.offset, ptr, range.size);
   }

   vkUnmapMemory(mem->device, range.memory);

   return 0;
}

static int
vkr_context_transfer_3d(struct virgl_context *base,
                        struct virgl_resource *res,
                        const struct vrend_transfer_info *info,
                        int transfer_mode)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   int ret;

   mtx_lock(&ctx->mutex);
   ret = vkr_context_transfer_3d_locked(base, res, info, transfer_mode);
   mtx_unlock(&ctx->mutex);

   return ret;
}

static void
vkr_context_attach_resource_locked(struct virgl_context *base, struct virgl_resource *res)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   struct vkr_resource_attachment *att;

   att = util_hash_table_get(ctx->resource_table, uintptr_to_pointer(res->res_id));
   if (att) {
      assert(att->resource == res);
      return;
   }

   att = calloc(1, sizeof(*att));
   if (!att)
      return;

   /* TODO When in multi-process mode, we cannot share a virgl_resource as-is
    * to another process.  The resource must have a valid fd, and only the fd
    * and the iov can be sent the other process.
    *
    * For vrend-to-vkr sharing, we can get the fd from pipe_resource.
    */

   att->resource = res;
   list_inithead(&att->memories);

   /* associate a memory with the resource, if any */
   struct vkr_device_memory *mem;
   LIST_FOR_EACH_ENTRY (mem, &ctx->newly_exported_memories, head) {
      if (mem->exported_res_id == res->res_id) {
         list_del(&mem->head);
         list_addtail(&mem->head, &att->memories);
         break;
      }
   }

   util_hash_table_set(ctx->resource_table, uintptr_to_pointer(res->res_id), att);
}

static void
vkr_context_attach_resource(struct virgl_context *base, struct virgl_resource *res)
{
   struct vkr_context *ctx = (struct vkr_context *)base;
   mtx_lock(&ctx->mutex);
   vkr_context_attach_resource_locked(base, res);
   mtx_unlock(&ctx->mutex);
}

static void
vkr_context_detach_resource(struct virgl_context *base, struct virgl_resource *res)
{
   struct vkr_context *ctx = (struct vkr_context *)base;

   mtx_lock(&ctx->mutex);
   util_hash_table_remove(ctx->resource_table, uintptr_to_pointer(res->res_id));
   mtx_unlock(&ctx->mutex);
}

static void
vkr_context_destroy(struct virgl_context *base)
{
   /* TODO Move the entire teardown process to a separate thread so that the main thread
    * cannot get blocked by the vkDeviceWaitIdle upon device destruction.
    */
   struct vkr_context *ctx = (struct vkr_context *)base;

   struct vkr_ring *ring, *ring_tmp;
   LIST_FOR_EACH_ENTRY_SAFE (ring, ring_tmp, &ctx->rings, head) {
      vkr_ring_stop(ring);
      vkr_ring_destroy(ring);
   }

   if (ctx->instance) {
      vrend_printf("destroying context with a valid instance");

      vkr_instance_destroy(ctx, ctx->instance);
   }

   util_hash_table_destroy(ctx->resource_table);
   util_hash_table_destroy_u64(ctx->object_table);

   if (ctx->fence_eventfd >= 0)
      close(ctx->fence_eventfd);

   vkr_cs_decoder_fini(&ctx->decoder);

   mtx_destroy(&ctx->mutex);
   free(ctx->debug_name);
   free(ctx);
}

static void
vkr_context_init_base(struct vkr_context *ctx)
{
   ctx->base.destroy = vkr_context_destroy;
   ctx->base.attach_resource = vkr_context_attach_resource;
   ctx->base.detach_resource = vkr_context_detach_resource;
   ctx->base.transfer_3d = vkr_context_transfer_3d;
   ctx->base.get_blob = vkr_context_get_blob;
   ctx->base.get_blob_done = vkr_context_get_blob_done;
   ctx->base.submit_cmd = vkr_context_submit_cmd;

   ctx->base.get_fencing_fd = vkr_context_get_fencing_fd;
   ctx->base.retire_fences = vkr_context_retire_fences;
   ctx->base.submit_fence = vkr_context_submit_fence;
}

static void
destroy_func_object(void *val)
{
   struct vkr_object *obj = val;
   free(obj);
}

static void
destroy_func_resource(void *val)
{
   struct vkr_resource_attachment *att = val;
   struct vkr_device_memory *mem, *tmp;

   LIST_FOR_EACH_ENTRY_SAFE (mem, tmp, &att->memories, head)
      list_delinit(&mem->head);

   free(att);
}

struct virgl_context *
vkr_context_create(size_t debug_len, const char *debug_name)
{
   struct vkr_context *ctx;

   /* TODO inject a proxy context when multi-process */

   ctx = calloc(1, sizeof(*ctx));
   if (!ctx)
      return NULL;

   ctx->debug_name = malloc(debug_len + 1);
   if (!ctx->debug_name) {
      free(ctx);
      return NULL;
   }

   memcpy(ctx->debug_name, debug_name, debug_len);
   ctx->debug_name[debug_len] = '\0';

#ifdef ENABLE_VENUS_VALIDATE
   ctx->validate_level = VKR_CONTEXT_VALIDATE_ON;
   ctx->validate_fatal = false; /* TODO set this to true */
#else
   ctx->validate_level = VKR_CONTEXT_VALIDATE_NONE;
   ctx->validate_fatal = false;
#endif
   if (VKR_DEBUG(VALIDATE))
      ctx->validate_level = VKR_CONTEXT_VALIDATE_FULL;

   if (mtx_init(&ctx->mutex, mtx_plain) != thrd_success) {
      free(ctx->debug_name);
      free(ctx);
      return NULL;
   }

   list_inithead(&ctx->rings);

   ctx->object_table = util_hash_table_create_u64(destroy_func_object);
   ctx->resource_table =
      util_hash_table_create(hash_func_u32, compare_func, destroy_func_resource);
   if (!ctx->object_table || !ctx->resource_table)
      goto fail;

   list_inithead(&ctx->newly_exported_memories);

   vkr_cs_decoder_init(&ctx->decoder, ctx->object_table);
   vkr_cs_encoder_init(&ctx->encoder, &ctx->decoder.fatal_error);

   vkr_context_init_base(ctx);
   vkr_context_init_dispatch(ctx);

   if ((vkr_renderer_flags & VKR_RENDERER_THREAD_SYNC) &&
       !(vkr_renderer_flags & VKR_RENDERER_ASYNC_FENCE_CB)) {
      ctx->fence_eventfd = create_eventfd(0);
      if (ctx->fence_eventfd < 0)
         goto fail;
   } else {
      ctx->fence_eventfd = -1;
   }

   list_inithead(&ctx->busy_queues);

   return &ctx->base;

fail:
   if (ctx->object_table)
      util_hash_table_destroy_u64(ctx->object_table);
   if (ctx->resource_table)
      util_hash_table_destroy(ctx->resource_table);
   mtx_destroy(&ctx->mutex);
   free(ctx->debug_name);
   free(ctx);
   return NULL;
}

size_t
vkr_get_capset(void *capset)
{
   struct virgl_renderer_capset_venus *c = capset;
   if (c) {
      memset(c, 0, sizeof(*c));
      c->wire_format_version = vn_info_wire_format_version();
      c->vk_xml_version = vn_info_vk_xml_version();
      c->vk_ext_command_serialization_spec_version =
         vn_info_extension_spec_version("VK_EXT_command_serialization");
      c->vk_mesa_venus_protocol_spec_version =
         vn_info_extension_spec_version("VK_MESA_venus_protocol");
   }

   return sizeof(*c);
}

int
vkr_renderer_init(uint32_t flags)
{
   /* TODO VKR_RENDERER_MULTI_PROCESS hint */

   if ((vkr_renderer_flags & VKR_RENDERER_ASYNC_FENCE_CB) &&
       !(vkr_renderer_flags & VKR_RENDERER_THREAD_SYNC))
      return -EINVAL;

   vkr_renderer_flags = flags;
   vkr_debug_flags = debug_get_flags_option("VKR_DEBUG", vkr_debug_options, 0);

   return 0;
}

void
vkr_renderer_fini(void)
{
   vkr_renderer_flags = 0;
   vkr_debug_flags = 0;
}

void
vkr_renderer_reset(void)
{
}
