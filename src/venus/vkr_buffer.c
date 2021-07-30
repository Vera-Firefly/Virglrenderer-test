/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_buffer.h"

#include "venus-protocol/vn_protocol_renderer_buffer.h"
#include "venus-protocol/vn_protocol_renderer_buffer_view.h"

#include "vkr_context.h"
#include "vkr_device.h"
#include "vkr_physical_device.h"

static void
vkr_dispatch_vkCreateBuffer(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkCreateBuffer *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

#ifdef FORCE_ENABLE_DMABUF
   VkExternalMemoryBufferCreateInfo local_external_info;
   if (dev->physical_device->EXT_external_memory_dma_buf) {
      VkExternalMemoryBufferCreateInfo *external_info = vkr_find_pnext(
         args->pCreateInfo->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
      if (external_info) {
         external_info->handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      } else {
         local_external_info = (const VkExternalMemoryBufferCreateInfo){
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
            .pNext = args->pCreateInfo->pNext,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
         };
         ((VkBufferCreateInfo *)args->pCreateInfo)->pNext = &local_external_info;
      }
   }
#endif

   CREATE_OBJECT(buf, buffer, BUFFER, vkCreateBuffer, pBuffer);

   util_hash_table_set_u64(ctx->object_table, buf->base.id, buf);
}

static void
vkr_dispatch_vkDestroyBuffer(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkDestroyBuffer *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(buf, buffer, BUFFER, vkDestroyBuffer, buffer);

   util_hash_table_remove_u64(ctx->object_table, buf->base.id);
}

static void
vkr_dispatch_vkGetBufferMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetBufferMemoryRequirements *args)
{
   vn_replace_vkGetBufferMemoryRequirements_args_handle(args);
   vkGetBufferMemoryRequirements(args->device, args->buffer, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetBufferMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetBufferMemoryRequirements2 *args)
{
   vn_replace_vkGetBufferMemoryRequirements2_args_handle(args);
   vkGetBufferMemoryRequirements2(args->device, args->pInfo, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkBindBufferMemory(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkBindBufferMemory *args)
{
   vn_replace_vkBindBufferMemory_args_handle(args);
   args->ret =
      vkBindBufferMemory(args->device, args->buffer, args->memory, args->memoryOffset);
}

static void
vkr_dispatch_vkBindBufferMemory2(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkBindBufferMemory2 *args)
{
   vn_replace_vkBindBufferMemory2_args_handle(args);
   args->ret = vkBindBufferMemory2(args->device, args->bindInfoCount, args->pBindInfos);
}

static void
vkr_dispatch_vkGetBufferOpaqueCaptureAddress(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetBufferOpaqueCaptureAddress *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkGetBufferOpaqueCaptureAddress_args_handle(args);
   args->ret = dev->GetBufferOpaqueCaptureAddress(args->device, args->pInfo);
}

static void
vkr_dispatch_vkGetBufferDeviceAddress(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkGetBufferDeviceAddress *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkGetBufferDeviceAddress_args_handle(args);
   args->ret = dev->GetBufferDeviceAddress(args->device, args->pInfo);
}

static void
vkr_dispatch_vkCreateBufferView(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCreateBufferView *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(view, buffer_view, BUFFER_VIEW, vkCreateBufferView, pView);

   util_hash_table_set_u64(ctx->object_table, view->base.id, view);
}

static void
vkr_dispatch_vkDestroyBufferView(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkDestroyBufferView *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(view, buffer_view, BUFFER_VIEW, vkDestroyBufferView, bufferView);

   util_hash_table_remove_u64(ctx->object_table, view->base.id);
}

void
vkr_context_init_buffer_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateBuffer = vkr_dispatch_vkCreateBuffer;
   dispatch->dispatch_vkDestroyBuffer = vkr_dispatch_vkDestroyBuffer;
   dispatch->dispatch_vkGetBufferMemoryRequirements =
      vkr_dispatch_vkGetBufferMemoryRequirements;
   dispatch->dispatch_vkGetBufferMemoryRequirements2 =
      vkr_dispatch_vkGetBufferMemoryRequirements2;
   dispatch->dispatch_vkBindBufferMemory = vkr_dispatch_vkBindBufferMemory;
   dispatch->dispatch_vkBindBufferMemory2 = vkr_dispatch_vkBindBufferMemory2;
   dispatch->dispatch_vkGetBufferOpaqueCaptureAddress =
      vkr_dispatch_vkGetBufferOpaqueCaptureAddress;
   dispatch->dispatch_vkGetBufferDeviceAddress = vkr_dispatch_vkGetBufferDeviceAddress;
}

void
vkr_context_init_buffer_view_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateBufferView = vkr_dispatch_vkCreateBufferView;
   dispatch->dispatch_vkDestroyBufferView = vkr_dispatch_vkDestroyBufferView;
}
