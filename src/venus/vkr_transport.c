/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_transport.h"

#include "venus-protocol/vn_protocol_renderer_dispatches.h"
#include "venus-protocol/vn_protocol_renderer_transport.h"
#include "vrend_iov.h"

#include "vkr_context.h"
#include "vkr_ring.h"

static void
vkr_dispatch_vkSetReplyCommandStreamMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkSetReplyCommandStreamMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_resource_attachment *att;

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

   if (!args->streamCount) {
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

static bool
vkr_ring_layout_init(struct vkr_ring_layout *layout, const VkRingCreateInfoMESA *info)
{
   *layout = (struct vkr_ring_layout){
      .head = { info->headOffset, sizeof(uint32_t) },
      .tail = { info->tailOffset, sizeof(uint32_t) },
      .status = { info->statusOffset, sizeof(uint32_t) },
      .buffer = { info->bufferOffset, info->bufferSize },
      .extra = { info->extraOffset, info->extraSize },
   };

   const struct memory_region *regions[] = {
      &layout->head,
      &layout->tail,
      &layout->status,
      &layout->buffer,
      &layout->extra,
   };

   for (size_t i = 0; i < ARRAY_SIZE(regions); i++) {
      if (regions[i]->offset > info->size ||
          regions[i]->size > info->size - regions[i]->offset) {
         vkr_log("ring buffer control variable (offset=%lu, size=%lu) placed"
                 " out-of-bounds in shared memory layout",
                 regions[i]->offset, regions[i]->size);
         return false;
      }

      if (regions[i]->offset & 0x3) {
         vkr_log("ring buffer control variable (offset=%lu, size=%lu) must be"
                 " 32-bit aligned in shared memory layout",
                 regions[i]->offset, regions[i]->size);
         return false;
      }
   }

   /* assumes region->size == 0 is valid */
   for (size_t i = 0; i < ARRAY_SIZE(regions); i++) {
      if (!regions[i]->size)
         continue;

      for (size_t j = i + 1; j < ARRAY_SIZE(regions); j++) {
         if (!regions[j]->size)
            continue;

         if (regions[i]->offset < regions[j]->offset + regions[j]->size &&
             regions[j]->offset < regions[i]->offset + regions[i]->size) {
            vkr_log("ring buffer control variable (offset=%lu, size=%lu)"
                    " overlaps with control variable (offset=%lu, size=%lu)",
                    regions[j]->offset, regions[j]->size, regions[i]->offset,
                    regions[i]->size);
            return false;
         }
      }
   }

   if (!layout->buffer.size || !util_is_power_of_two(layout->buffer.size)) {
      vkr_log("ring buffer size (%lu) must be a power of two",
              layout->buffer.size);
      return false;
   }

   return true;
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

   att = util_hash_table_get(ctx->resource_table, uintptr_to_pointer(info->resourceId));
   if (!att) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   /* TODO support scatter-gather or require logically contiguous resources */
   if (att->resource->iov_count != 1) {
      vkr_log("no scatter-gather support for ring buffers (TODO)");
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

   struct vkr_ring_layout layout;
   if (!vkr_ring_layout_init(&layout, info)) {
      vkr_log("vkCreateRingMESA supplied with invalid buffer layout parameters");
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

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
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetVenusExperimentalFeatureData100000MESA *args)
{
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

void
vkr_context_init_transport_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

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

   dispatch->dispatch_vkGetVenusExperimentalFeatureData100000MESA =
      vkr_dispatch_vkGetVenusExperimentalFeatureData100000MESA;
}
