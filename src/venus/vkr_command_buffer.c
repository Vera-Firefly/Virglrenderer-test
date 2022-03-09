/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_command_buffer.h"

#include "vkr_command_buffer_gen.h"

static void
vkr_dispatch_vkCreateCommandPool(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCreateCommandPool *args)
{
   struct vkr_command_pool *pool = vkr_command_pool_create_and_add(dispatch->data, args);
   if (!pool)
      return;

   list_inithead(&pool->command_buffers);
}

static void
vkr_dispatch_vkDestroyCommandPool(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkDestroyCommandPool *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_command_pool *pool = vkr_command_pool_from_handle(args->commandPool);

   if (!pool)
      return;

   vkr_command_pool_release(ctx, pool);
   vkr_command_pool_destroy_and_remove(ctx, args);
}

static void
vkr_dispatch_vkResetCommandPool(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkResetCommandPool *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkResetCommandPool_args_handle(args);
   args->ret = vk->ResetCommandPool(args->device, args->commandPool, args->flags);
}

static void
vkr_dispatch_vkTrimCommandPool(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkTrimCommandPool *args)
{
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vn_device_proc_table *vk = &dev->proc_table;

   vn_replace_vkTrimCommandPool_args_handle(args);
   vk->TrimCommandPool(args->device, args->commandPool, args->flags);
}

static void
vkr_dispatch_vkAllocateCommandBuffers(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkAllocateCommandBuffers *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct vkr_command_pool *pool =
      vkr_command_pool_from_handle(args->pAllocateInfo->commandPool);
   struct object_array arr;

   if (!pool) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   if (vkr_command_buffer_create_array(ctx, args, &arr) != VK_SUCCESS)
      return;

   vkr_command_buffer_add_array(ctx, dev, pool, &arr);
}

static void
vkr_dispatch_vkFreeCommandBuffers(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkFreeCommandBuffers *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct list_head free_list;

   /* args->pCommandBuffers is marked noautovalidity="true" */
   if (args->commandBufferCount && !args->pCommandBuffers) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vkr_command_buffer_destroy_driver_handles(ctx, args, &free_list);
   vkr_context_remove_objects(ctx, &free_list);
}

static void
vkr_dispatch_vkResetCommandBuffer(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkResetCommandBuffer *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkResetCommandBuffer_args_handle(args);
   args->ret = vk->ResetCommandBuffer(args->commandBuffer, args->flags);
}

static void
vkr_dispatch_vkBeginCommandBuffer(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkBeginCommandBuffer *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkBeginCommandBuffer_args_handle(args);
   args->ret = vk->BeginCommandBuffer(args->commandBuffer, args->pBeginInfo);
}

static void
vkr_dispatch_vkEndCommandBuffer(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkEndCommandBuffer *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkEndCommandBuffer_args_handle(args);
   args->ret = vk->EndCommandBuffer(args->commandBuffer);
}

static void
vkr_dispatch_vkCmdBindPipeline(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdBindPipeline *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdBindPipeline_args_handle(args);
   vk->CmdBindPipeline(args->commandBuffer, args->pipelineBindPoint, args->pipeline);
}

static void
vkr_dispatch_vkCmdSetViewport(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCmdSetViewport *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetViewport_args_handle(args);
   vk->CmdSetViewport(args->commandBuffer, args->firstViewport, args->viewportCount,
                      args->pViewports);
}

static void
vkr_dispatch_vkCmdSetScissor(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdSetScissor *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetScissor_args_handle(args);
   vk->CmdSetScissor(args->commandBuffer, args->firstScissor, args->scissorCount,
                     args->pScissors);
}

static void
vkr_dispatch_vkCmdSetLineWidth(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdSetLineWidth *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetLineWidth_args_handle(args);
   vk->CmdSetLineWidth(args->commandBuffer, args->lineWidth);
}

static void
vkr_dispatch_vkCmdSetDepthBias(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdSetDepthBias *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetDepthBias_args_handle(args);
   vk->CmdSetDepthBias(args->commandBuffer, args->depthBiasConstantFactor,
                       args->depthBiasClamp, args->depthBiasSlopeFactor);
}

static void
vkr_dispatch_vkCmdSetBlendConstants(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdSetBlendConstants *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetBlendConstants_args_handle(args);
   vk->CmdSetBlendConstants(args->commandBuffer, args->blendConstants);
}

static void
vkr_dispatch_vkCmdSetDepthBounds(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCmdSetDepthBounds *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetDepthBounds_args_handle(args);
   vk->CmdSetDepthBounds(args->commandBuffer, args->minDepthBounds, args->maxDepthBounds);
}

static void
vkr_dispatch_vkCmdSetStencilCompareMask(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkCmdSetStencilCompareMask *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetStencilCompareMask_args_handle(args);
   vk->CmdSetStencilCompareMask(args->commandBuffer, args->faceMask, args->compareMask);
}

static void
vkr_dispatch_vkCmdSetStencilWriteMask(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdSetStencilWriteMask *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetStencilWriteMask_args_handle(args);
   vk->CmdSetStencilWriteMask(args->commandBuffer, args->faceMask, args->writeMask);
}

static void
vkr_dispatch_vkCmdSetStencilReference(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdSetStencilReference *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetStencilReference_args_handle(args);
   vk->CmdSetStencilReference(args->commandBuffer, args->faceMask, args->reference);
}

static void
vkr_dispatch_vkCmdBindDescriptorSets(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdBindDescriptorSets *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdBindDescriptorSets_args_handle(args);
   vk->CmdBindDescriptorSets(args->commandBuffer, args->pipelineBindPoint, args->layout,
                             args->firstSet, args->descriptorSetCount,
                             args->pDescriptorSets, args->dynamicOffsetCount,
                             args->pDynamicOffsets);
}

static void
vkr_dispatch_vkCmdBindIndexBuffer(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCmdBindIndexBuffer *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdBindIndexBuffer_args_handle(args);
   vk->CmdBindIndexBuffer(args->commandBuffer, args->buffer, args->offset,
                          args->indexType);
}

static void
vkr_dispatch_vkCmdBindVertexBuffers(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdBindVertexBuffers *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdBindVertexBuffers_args_handle(args);
   vk->CmdBindVertexBuffers(args->commandBuffer, args->firstBinding, args->bindingCount,
                            args->pBuffers, args->pOffsets);
}

static void
vkr_dispatch_vkCmdDraw(UNUSED struct vn_dispatch_context *dispatch,
                       struct vn_command_vkCmdDraw *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdDraw_args_handle(args);
   vk->CmdDraw(args->commandBuffer, args->vertexCount, args->instanceCount,
               args->firstVertex, args->firstInstance);
}

static void
vkr_dispatch_vkCmdDrawIndexed(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCmdDrawIndexed *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdDrawIndexed_args_handle(args);
   vk->CmdDrawIndexed(args->commandBuffer, args->indexCount, args->instanceCount,
                      args->firstIndex, args->vertexOffset, args->firstInstance);
}

static void
vkr_dispatch_vkCmdDrawIndirect(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdDrawIndirect *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdDrawIndirect_args_handle(args);
   vk->CmdDrawIndirect(args->commandBuffer, args->buffer, args->offset, args->drawCount,
                       args->stride);
}

static void
vkr_dispatch_vkCmdDrawIndexedIndirect(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdDrawIndexedIndirect *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdDrawIndexedIndirect_args_handle(args);
   vk->CmdDrawIndexedIndirect(args->commandBuffer, args->buffer, args->offset,
                              args->drawCount, args->stride);
}

static void
vkr_dispatch_vkCmdDispatch(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCmdDispatch *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdDispatch_args_handle(args);
   vk->CmdDispatch(args->commandBuffer, args->groupCountX, args->groupCountY,
                   args->groupCountZ);
}

static void
vkr_dispatch_vkCmdDispatchIndirect(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCmdDispatchIndirect *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdDispatchIndirect_args_handle(args);
   vk->CmdDispatchIndirect(args->commandBuffer, args->buffer, args->offset);
}

static void
vkr_dispatch_vkCmdCopyBuffer(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdCopyBuffer *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdCopyBuffer_args_handle(args);
   vk->CmdCopyBuffer(args->commandBuffer, args->srcBuffer, args->dstBuffer,
                     args->regionCount, args->pRegions);
}

static void
vkr_dispatch_vkCmdCopyImage(UNUSED struct vn_dispatch_context *dispatch,
                            struct vn_command_vkCmdCopyImage *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdCopyImage_args_handle(args);
   vk->CmdCopyImage(args->commandBuffer, args->srcImage, args->srcImageLayout,
                    args->dstImage, args->dstImageLayout, args->regionCount,
                    args->pRegions);
}

static void
vkr_dispatch_vkCmdBlitImage(UNUSED struct vn_dispatch_context *dispatch,
                            struct vn_command_vkCmdBlitImage *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdBlitImage_args_handle(args);
   vk->CmdBlitImage(args->commandBuffer, args->srcImage, args->srcImageLayout,
                    args->dstImage, args->dstImageLayout, args->regionCount,
                    args->pRegions, args->filter);
}

static void
vkr_dispatch_vkCmdCopyBufferToImage(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdCopyBufferToImage *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdCopyBufferToImage_args_handle(args);
   vk->CmdCopyBufferToImage(args->commandBuffer, args->srcBuffer, args->dstImage,
                            args->dstImageLayout, args->regionCount, args->pRegions);
}

static void
vkr_dispatch_vkCmdCopyImageToBuffer(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdCopyImageToBuffer *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdCopyImageToBuffer_args_handle(args);
   vk->CmdCopyImageToBuffer(args->commandBuffer, args->srcImage, args->srcImageLayout,
                            args->dstBuffer, args->regionCount, args->pRegions);
}

static void
vkr_dispatch_vkCmdUpdateBuffer(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdUpdateBuffer *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdUpdateBuffer_args_handle(args);
   vk->CmdUpdateBuffer(args->commandBuffer, args->dstBuffer, args->dstOffset,
                       args->dataSize, args->pData);
}

static void
vkr_dispatch_vkCmdFillBuffer(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdFillBuffer *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdFillBuffer_args_handle(args);
   vk->CmdFillBuffer(args->commandBuffer, args->dstBuffer, args->dstOffset, args->size,
                     args->data);
}

static void
vkr_dispatch_vkCmdClearColorImage(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCmdClearColorImage *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdClearColorImage_args_handle(args);
   vk->CmdClearColorImage(args->commandBuffer, args->image, args->imageLayout,
                          args->pColor, args->rangeCount, args->pRanges);
}

static void
vkr_dispatch_vkCmdClearDepthStencilImage(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdClearDepthStencilImage *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdClearDepthStencilImage_args_handle(args);
   vk->CmdClearDepthStencilImage(args->commandBuffer, args->image, args->imageLayout,
                                 args->pDepthStencil, args->rangeCount, args->pRanges);
}

static void
vkr_dispatch_vkCmdClearAttachments(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCmdClearAttachments *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdClearAttachments_args_handle(args);
   vk->CmdClearAttachments(args->commandBuffer, args->attachmentCount, args->pAttachments,
                           args->rectCount, args->pRects);
}

static void
vkr_dispatch_vkCmdResolveImage(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdResolveImage *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdResolveImage_args_handle(args);
   vk->CmdResolveImage(args->commandBuffer, args->srcImage, args->srcImageLayout,
                       args->dstImage, args->dstImageLayout, args->regionCount,
                       args->pRegions);
}

static void
vkr_dispatch_vkCmdSetEvent(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCmdSetEvent *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetEvent_args_handle(args);
   vk->CmdSetEvent(args->commandBuffer, args->event, args->stageMask);
}

static void
vkr_dispatch_vkCmdResetEvent(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdResetEvent *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdResetEvent_args_handle(args);
   vk->CmdResetEvent(args->commandBuffer, args->event, args->stageMask);
}

static void
vkr_dispatch_vkCmdWaitEvents(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdWaitEvents *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdWaitEvents_args_handle(args);
   vk->CmdWaitEvents(args->commandBuffer, args->eventCount, args->pEvents,
                     args->srcStageMask, args->dstStageMask, args->memoryBarrierCount,
                     args->pMemoryBarriers, args->bufferMemoryBarrierCount,
                     args->pBufferMemoryBarriers, args->imageMemoryBarrierCount,
                     args->pImageMemoryBarriers);
}

static void
vkr_dispatch_vkCmdPipelineBarrier(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCmdPipelineBarrier *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdPipelineBarrier_args_handle(args);
   vk->CmdPipelineBarrier(args->commandBuffer, args->srcStageMask, args->dstStageMask,
                          args->dependencyFlags, args->memoryBarrierCount,
                          args->pMemoryBarriers, args->bufferMemoryBarrierCount,
                          args->pBufferMemoryBarriers, args->imageMemoryBarrierCount,
                          args->pImageMemoryBarriers);
}

static void
vkr_dispatch_vkCmdBeginQuery(UNUSED struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCmdBeginQuery *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdBeginQuery_args_handle(args);
   vk->CmdBeginQuery(args->commandBuffer, args->queryPool, args->query, args->flags);
}

static void
vkr_dispatch_vkCmdEndQuery(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCmdEndQuery *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdEndQuery_args_handle(args);
   vk->CmdEndQuery(args->commandBuffer, args->queryPool, args->query);
}

static void
vkr_dispatch_vkCmdResetQueryPool(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCmdResetQueryPool *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdResetQueryPool_args_handle(args);
   vk->CmdResetQueryPool(args->commandBuffer, args->queryPool, args->firstQuery,
                         args->queryCount);
}

static void
vkr_dispatch_vkCmdWriteTimestamp(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCmdWriteTimestamp *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdWriteTimestamp_args_handle(args);
   vk->CmdWriteTimestamp(args->commandBuffer, args->pipelineStage, args->queryPool,
                         args->query);
}

static void
vkr_dispatch_vkCmdCopyQueryPoolResults(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdCopyQueryPoolResults *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdCopyQueryPoolResults_args_handle(args);
   vk->CmdCopyQueryPoolResults(args->commandBuffer, args->queryPool, args->firstQuery,
                               args->queryCount, args->dstBuffer, args->dstOffset,
                               args->stride, args->flags);
}

static void
vkr_dispatch_vkCmdPushConstants(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCmdPushConstants *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdPushConstants_args_handle(args);
   vk->CmdPushConstants(args->commandBuffer, args->layout, args->stageFlags, args->offset,
                        args->size, args->pValues);
}

static void
vkr_dispatch_vkCmdBeginRenderPass(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCmdBeginRenderPass *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdBeginRenderPass_args_handle(args);
   vk->CmdBeginRenderPass(args->commandBuffer, args->pRenderPassBegin, args->contents);
}

static void
vkr_dispatch_vkCmdNextSubpass(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCmdNextSubpass *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdNextSubpass_args_handle(args);
   vk->CmdNextSubpass(args->commandBuffer, args->contents);
}

static void
vkr_dispatch_vkCmdEndRenderPass(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCmdEndRenderPass *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdEndRenderPass_args_handle(args);
   vk->CmdEndRenderPass(args->commandBuffer);
}

static void
vkr_dispatch_vkCmdExecuteCommands(UNUSED struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCmdExecuteCommands *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdExecuteCommands_args_handle(args);
   vk->CmdExecuteCommands(args->commandBuffer, args->commandBufferCount,
                          args->pCommandBuffers);
}

static void
vkr_dispatch_vkCmdSetDeviceMask(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCmdSetDeviceMask *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetDeviceMask_args_handle(args);
   vk->CmdSetDeviceMask(args->commandBuffer, args->deviceMask);
}

static void
vkr_dispatch_vkCmdDispatchBase(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdDispatchBase *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdDispatchBase_args_handle(args);
   vk->CmdDispatchBase(args->commandBuffer, args->baseGroupX, args->baseGroupY,
                       args->baseGroupZ, args->groupCountX, args->groupCountY,
                       args->groupCountZ);
}

static void
vkr_dispatch_vkCmdBeginRenderPass2(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCmdBeginRenderPass2 *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdBeginRenderPass2_args_handle(args);
   vk->CmdBeginRenderPass2(args->commandBuffer, args->pRenderPassBegin,
                           args->pSubpassBeginInfo);
}

static void
vkr_dispatch_vkCmdNextSubpass2(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdNextSubpass2 *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdNextSubpass2_args_handle(args);
   vk->CmdNextSubpass2(args->commandBuffer, args->pSubpassBeginInfo,
                       args->pSubpassEndInfo);
}

static void
vkr_dispatch_vkCmdEndRenderPass2(UNUSED struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCmdEndRenderPass2 *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdEndRenderPass2_args_handle(args);
   vk->CmdEndRenderPass2(args->commandBuffer, args->pSubpassEndInfo);
}

static void
vkr_dispatch_vkCmdDrawIndirectCount(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdDrawIndirectCount *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdDrawIndirectCount_args_handle(args);
   vk->CmdDrawIndirectCount(args->commandBuffer, args->buffer, args->offset,
                            args->countBuffer, args->countBufferOffset,
                            args->maxDrawCount, args->stride);
}

static void
vkr_dispatch_vkCmdDrawIndexedIndirectCount(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdDrawIndexedIndirectCount *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);

   vn_replace_vkCmdDrawIndexedIndirectCount_args_handle(args);
   cmd->device->proc_table.CmdDrawIndexedIndirectCount(
      args->commandBuffer, args->buffer, args->offset, args->countBuffer,
      args->countBufferOffset, args->maxDrawCount, args->stride);
}

static void
vkr_dispatch_vkCmdSetLineStippleEXT(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdSetLineStippleEXT *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetLineStippleEXT_args_handle(args);
   vk->CmdSetLineStippleEXT(args->commandBuffer, args->lineStippleFactor,
                            args->lineStipplePattern);
}

static void
vkr_dispatch_vkCmdBindTransformFeedbackBuffersEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdBindTransformFeedbackBuffersEXT *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);

   vn_replace_vkCmdBindTransformFeedbackBuffersEXT_args_handle(args);
   cmd->device->proc_table.CmdBindTransformFeedbackBuffersEXT(
      args->commandBuffer, args->firstBinding, args->bindingCount, args->pBuffers,
      args->pOffsets, args->pSizes);
}

static void
vkr_dispatch_vkCmdBeginTransformFeedbackEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdBeginTransformFeedbackEXT *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);

   vn_replace_vkCmdBeginTransformFeedbackEXT_args_handle(args);
   cmd->device->proc_table.CmdBeginTransformFeedbackEXT(
      args->commandBuffer, args->firstCounterBuffer, args->counterBufferCount,
      args->pCounterBuffers, args->pCounterBufferOffsets);
}

static void
vkr_dispatch_vkCmdEndTransformFeedbackEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdEndTransformFeedbackEXT *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);

   vn_replace_vkCmdEndTransformFeedbackEXT_args_handle(args);
   cmd->device->proc_table.CmdEndTransformFeedbackEXT(
      args->commandBuffer, args->firstCounterBuffer, args->counterBufferCount,
      args->pCounterBuffers, args->pCounterBufferOffsets);
}

static void
vkr_dispatch_vkCmdBeginQueryIndexedEXT(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdBeginQueryIndexedEXT *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdBeginQueryIndexedEXT_args_handle(args);
   vk->CmdBeginQueryIndexedEXT(args->commandBuffer, args->queryPool, args->query,
                               args->flags, args->index);
}

static void
vkr_dispatch_vkCmdEndQueryIndexedEXT(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdEndQueryIndexedEXT *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdEndQueryIndexedEXT_args_handle(args);
   vk->CmdEndQueryIndexedEXT(args->commandBuffer, args->queryPool, args->query,
                             args->index);
}

static void
vkr_dispatch_vkCmdDrawIndirectByteCountEXT(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdDrawIndirectByteCountEXT *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);

   vn_replace_vkCmdDrawIndirectByteCountEXT_args_handle(args);
   cmd->device->proc_table.CmdDrawIndirectByteCountEXT(
      args->commandBuffer, args->instanceCount, args->firstInstance, args->counterBuffer,
      args->counterBufferOffset, args->counterOffset, args->vertexStride);
}

static void
vkr_dispatch_vkCmdBindVertexBuffers2(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdBindVertexBuffers2 *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdBindVertexBuffers2_args_handle(args);
   vk->CmdBindVertexBuffers2(args->commandBuffer, args->firstBinding, args->bindingCount,
                             args->pBuffers, args->pOffsets, args->pSizes,
                             args->pStrides);
}

static void
vkr_dispatch_vkCmdSetCullMode(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCmdSetCullMode *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);

   vn_replace_vkCmdSetCullMode_args_handle(args);
   cmd->device->proc_table.CmdSetCullMode(args->commandBuffer, args->cullMode);
}

static void
vkr_dispatch_vkCmdSetDepthBoundsTestEnable(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkCmdSetDepthBoundsTestEnable *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);

   vn_replace_vkCmdSetDepthBoundsTestEnable_args_handle(args);
   cmd->device->proc_table.CmdSetDepthBoundsTestEnable(args->commandBuffer,
                                                       args->depthBoundsTestEnable);
}

static void
vkr_dispatch_vkCmdSetDepthCompareOp(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCmdSetDepthCompareOp *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetDepthCompareOp_args_handle(args);
   vk->CmdSetDepthCompareOp(args->commandBuffer, args->depthCompareOp);
}

static void
vkr_dispatch_vkCmdSetDepthTestEnable(UNUSED struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkCmdSetDepthTestEnable *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetDepthTestEnable_args_handle(args);
   vk->CmdSetDepthTestEnable(args->commandBuffer, args->depthTestEnable);
}

static void
vkr_dispatch_vkCmdSetDepthWriteEnable(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdSetDepthWriteEnable *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetDepthWriteEnable_args_handle(args);
   vk->CmdSetDepthWriteEnable(args->commandBuffer, args->depthWriteEnable);
}

static void
vkr_dispatch_vkCmdSetFrontFace(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdSetFrontFace *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetFrontFace_args_handle(args);
   vk->CmdSetFrontFace(args->commandBuffer, args->frontFace);
}

static void
vkr_dispatch_vkCmdSetPrimitiveTopology(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdSetPrimitiveTopology *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetPrimitiveTopology_args_handle(args);
   vk->CmdSetPrimitiveTopology(args->commandBuffer, args->primitiveTopology);
}

static void
vkr_dispatch_vkCmdSetScissorWithCount(UNUSED struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCmdSetScissorWithCount *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetScissorWithCount_args_handle(args);
   vk->CmdSetScissorWithCount(args->commandBuffer, args->scissorCount, args->pScissors);
}

static void
vkr_dispatch_vkCmdSetStencilOp(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCmdSetStencilOp *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetStencilOp_args_handle(args);
   vk->CmdSetStencilOp(args->commandBuffer, args->faceMask, args->failOp, args->passOp,
                       args->depthFailOp, args->compareOp);
}

static void
vkr_dispatch_vkCmdSetStencilTestEnable(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdSetStencilTestEnable *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetStencilTestEnable_args_handle(args);
   vk->CmdSetStencilTestEnable(args->commandBuffer, args->stencilTestEnable);
}

static void
vkr_dispatch_vkCmdSetViewportWithCount(UNUSED struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCmdSetViewportWithCount *args)
{
   struct vkr_command_buffer *cmd = vkr_command_buffer_from_handle(args->commandBuffer);
   struct vn_device_proc_table *vk = &cmd->device->proc_table;

   vn_replace_vkCmdSetViewportWithCount_args_handle(args);
   vk->CmdSetViewportWithCount(args->commandBuffer, args->viewportCount,
                               args->pViewports);
}

void
vkr_context_init_command_pool_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateCommandPool = vkr_dispatch_vkCreateCommandPool;
   dispatch->dispatch_vkDestroyCommandPool = vkr_dispatch_vkDestroyCommandPool;
   dispatch->dispatch_vkResetCommandPool = vkr_dispatch_vkResetCommandPool;
   dispatch->dispatch_vkTrimCommandPool = vkr_dispatch_vkTrimCommandPool;
}

void
vkr_context_init_command_buffer_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkAllocateCommandBuffers = vkr_dispatch_vkAllocateCommandBuffers;
   dispatch->dispatch_vkFreeCommandBuffers = vkr_dispatch_vkFreeCommandBuffers;
   dispatch->dispatch_vkResetCommandBuffer = vkr_dispatch_vkResetCommandBuffer;
   dispatch->dispatch_vkBeginCommandBuffer = vkr_dispatch_vkBeginCommandBuffer;
   dispatch->dispatch_vkEndCommandBuffer = vkr_dispatch_vkEndCommandBuffer;

   dispatch->dispatch_vkCmdBindPipeline = vkr_dispatch_vkCmdBindPipeline;
   dispatch->dispatch_vkCmdSetViewport = vkr_dispatch_vkCmdSetViewport;
   dispatch->dispatch_vkCmdSetScissor = vkr_dispatch_vkCmdSetScissor;
   dispatch->dispatch_vkCmdSetLineWidth = vkr_dispatch_vkCmdSetLineWidth;
   dispatch->dispatch_vkCmdSetDepthBias = vkr_dispatch_vkCmdSetDepthBias;
   dispatch->dispatch_vkCmdSetBlendConstants = vkr_dispatch_vkCmdSetBlendConstants;
   dispatch->dispatch_vkCmdSetDepthBounds = vkr_dispatch_vkCmdSetDepthBounds;
   dispatch->dispatch_vkCmdSetStencilCompareMask =
      vkr_dispatch_vkCmdSetStencilCompareMask;
   dispatch->dispatch_vkCmdSetStencilWriteMask = vkr_dispatch_vkCmdSetStencilWriteMask;
   dispatch->dispatch_vkCmdSetStencilReference = vkr_dispatch_vkCmdSetStencilReference;
   dispatch->dispatch_vkCmdBindDescriptorSets = vkr_dispatch_vkCmdBindDescriptorSets;
   dispatch->dispatch_vkCmdBindIndexBuffer = vkr_dispatch_vkCmdBindIndexBuffer;
   dispatch->dispatch_vkCmdBindVertexBuffers = vkr_dispatch_vkCmdBindVertexBuffers;
   dispatch->dispatch_vkCmdDraw = vkr_dispatch_vkCmdDraw;
   dispatch->dispatch_vkCmdDrawIndexed = vkr_dispatch_vkCmdDrawIndexed;
   dispatch->dispatch_vkCmdDrawIndirect = vkr_dispatch_vkCmdDrawIndirect;
   dispatch->dispatch_vkCmdDrawIndexedIndirect = vkr_dispatch_vkCmdDrawIndexedIndirect;
   dispatch->dispatch_vkCmdDispatch = vkr_dispatch_vkCmdDispatch;
   dispatch->dispatch_vkCmdDispatchIndirect = vkr_dispatch_vkCmdDispatchIndirect;
   dispatch->dispatch_vkCmdCopyBuffer = vkr_dispatch_vkCmdCopyBuffer;
   dispatch->dispatch_vkCmdCopyImage = vkr_dispatch_vkCmdCopyImage;
   dispatch->dispatch_vkCmdBlitImage = vkr_dispatch_vkCmdBlitImage;
   dispatch->dispatch_vkCmdCopyBufferToImage = vkr_dispatch_vkCmdCopyBufferToImage;
   dispatch->dispatch_vkCmdCopyImageToBuffer = vkr_dispatch_vkCmdCopyImageToBuffer;
   dispatch->dispatch_vkCmdUpdateBuffer = vkr_dispatch_vkCmdUpdateBuffer;
   dispatch->dispatch_vkCmdFillBuffer = vkr_dispatch_vkCmdFillBuffer;
   dispatch->dispatch_vkCmdClearColorImage = vkr_dispatch_vkCmdClearColorImage;
   dispatch->dispatch_vkCmdClearDepthStencilImage =
      vkr_dispatch_vkCmdClearDepthStencilImage;
   dispatch->dispatch_vkCmdClearAttachments = vkr_dispatch_vkCmdClearAttachments;
   dispatch->dispatch_vkCmdResolveImage = vkr_dispatch_vkCmdResolveImage;
   dispatch->dispatch_vkCmdSetEvent = vkr_dispatch_vkCmdSetEvent;
   dispatch->dispatch_vkCmdResetEvent = vkr_dispatch_vkCmdResetEvent;
   dispatch->dispatch_vkCmdWaitEvents = vkr_dispatch_vkCmdWaitEvents;
   dispatch->dispatch_vkCmdPipelineBarrier = vkr_dispatch_vkCmdPipelineBarrier;
   dispatch->dispatch_vkCmdBeginQuery = vkr_dispatch_vkCmdBeginQuery;
   dispatch->dispatch_vkCmdEndQuery = vkr_dispatch_vkCmdEndQuery;
   dispatch->dispatch_vkCmdResetQueryPool = vkr_dispatch_vkCmdResetQueryPool;
   dispatch->dispatch_vkCmdWriteTimestamp = vkr_dispatch_vkCmdWriteTimestamp;
   dispatch->dispatch_vkCmdCopyQueryPoolResults = vkr_dispatch_vkCmdCopyQueryPoolResults;
   dispatch->dispatch_vkCmdPushConstants = vkr_dispatch_vkCmdPushConstants;
   dispatch->dispatch_vkCmdBeginRenderPass = vkr_dispatch_vkCmdBeginRenderPass;
   dispatch->dispatch_vkCmdNextSubpass = vkr_dispatch_vkCmdNextSubpass;
   dispatch->dispatch_vkCmdEndRenderPass = vkr_dispatch_vkCmdEndRenderPass;
   dispatch->dispatch_vkCmdExecuteCommands = vkr_dispatch_vkCmdExecuteCommands;
   dispatch->dispatch_vkCmdSetDeviceMask = vkr_dispatch_vkCmdSetDeviceMask;
   dispatch->dispatch_vkCmdDispatchBase = vkr_dispatch_vkCmdDispatchBase;
   dispatch->dispatch_vkCmdBeginRenderPass2 = vkr_dispatch_vkCmdBeginRenderPass2;
   dispatch->dispatch_vkCmdNextSubpass2 = vkr_dispatch_vkCmdNextSubpass2;
   dispatch->dispatch_vkCmdEndRenderPass2 = vkr_dispatch_vkCmdEndRenderPass2;
   dispatch->dispatch_vkCmdDrawIndirectCount = vkr_dispatch_vkCmdDrawIndirectCount;
   dispatch->dispatch_vkCmdDrawIndexedIndirectCount =
      vkr_dispatch_vkCmdDrawIndexedIndirectCount;

   dispatch->dispatch_vkCmdSetLineStippleEXT = vkr_dispatch_vkCmdSetLineStippleEXT;

   dispatch->dispatch_vkCmdBindTransformFeedbackBuffersEXT =
      vkr_dispatch_vkCmdBindTransformFeedbackBuffersEXT;
   dispatch->dispatch_vkCmdBeginTransformFeedbackEXT =
      vkr_dispatch_vkCmdBeginTransformFeedbackEXT;
   dispatch->dispatch_vkCmdEndTransformFeedbackEXT =
      vkr_dispatch_vkCmdEndTransformFeedbackEXT;
   dispatch->dispatch_vkCmdBeginQueryIndexedEXT = vkr_dispatch_vkCmdBeginQueryIndexedEXT;
   dispatch->dispatch_vkCmdEndQueryIndexedEXT = vkr_dispatch_vkCmdEndQueryIndexedEXT;
   dispatch->dispatch_vkCmdDrawIndirectByteCountEXT =
      vkr_dispatch_vkCmdDrawIndirectByteCountEXT;

   dispatch->dispatch_vkCmdBindVertexBuffers2 = vkr_dispatch_vkCmdBindVertexBuffers2;
   dispatch->dispatch_vkCmdSetCullMode = vkr_dispatch_vkCmdSetCullMode;
   dispatch->dispatch_vkCmdSetDepthBoundsTestEnable =
      vkr_dispatch_vkCmdSetDepthBoundsTestEnable;
   dispatch->dispatch_vkCmdSetDepthCompareOp = vkr_dispatch_vkCmdSetDepthCompareOp;
   dispatch->dispatch_vkCmdSetDepthTestEnable = vkr_dispatch_vkCmdSetDepthTestEnable;
   dispatch->dispatch_vkCmdSetDepthWriteEnable = vkr_dispatch_vkCmdSetDepthWriteEnable;
   dispatch->dispatch_vkCmdSetFrontFace = vkr_dispatch_vkCmdSetFrontFace;
   dispatch->dispatch_vkCmdSetPrimitiveTopology = vkr_dispatch_vkCmdSetPrimitiveTopology;
   dispatch->dispatch_vkCmdSetScissorWithCount = vkr_dispatch_vkCmdSetScissorWithCount;
   dispatch->dispatch_vkCmdSetStencilOp = vkr_dispatch_vkCmdSetStencilOp;
   dispatch->dispatch_vkCmdSetStencilTestEnable = vkr_dispatch_vkCmdSetStencilTestEnable;
   dispatch->dispatch_vkCmdSetViewportWithCount = vkr_dispatch_vkCmdSetViewportWithCount;
}
