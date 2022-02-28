/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_device.h"

#include "venus-protocol/vn_protocol_renderer_device.h"

#include "vkr_command_buffer.h"
#include "vkr_context.h"
#include "vkr_descriptor_set.h"
#include "vkr_device_memory.h"
#include "vkr_physical_device.h"
#include "vkr_queue.h"

static VkResult
vkr_device_create_queues(struct vkr_context *ctx,
                         struct vkr_device *dev,
                         uint32_t create_info_count,
                         const VkDeviceQueueCreateInfo *create_infos)
{
   list_inithead(&dev->queues);

   for (uint32_t i = 0; i < create_info_count; i++) {
      for (uint32_t j = 0; j < create_infos[i].queueCount; j++) {
         const VkDeviceQueueInfo2 info = {
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
            .pNext = NULL,
            .flags = create_infos[i].flags,
            .queueFamilyIndex = create_infos[i].queueFamilyIndex,
            .queueIndex = j,
         };
         VkQueue handle = VK_NULL_HANDLE;
         /* There was a bug in spec which forbids usage of vkGetDeviceQueue2
          * with flags set to zero. It was fixed in spec version 1.1.130.
          * Work around drivers that are implementing this buggy behavior
          */
         if (info.flags) {
            vkGetDeviceQueue2(dev->base.handle.device, &info, &handle);
         } else {
            vkGetDeviceQueue(dev->base.handle.device, info.queueFamilyIndex,
                             info.queueIndex, &handle);
         }

         struct vkr_queue *queue = vkr_queue_create(
            ctx, dev, info.flags, info.queueFamilyIndex, info.queueIndex, handle);
         if (!queue) {
            struct vkr_queue *entry, *tmp;
            LIST_FOR_EACH_ENTRY_SAFE (entry, tmp, &dev->queues, base.track_head)
               vkr_queue_destroy(ctx, entry);

            return VK_ERROR_OUT_OF_HOST_MEMORY;
         }

         /* queues are not tracked as device objects */
         list_add(&queue->base.track_head, &dev->queues);
      }
   }

   return VK_SUCCESS;
}

static void
vkr_device_init_entry_points(struct vkr_device *dev, uint32_t api_version)
{
   VkDevice handle = dev->base.handle.device;
   if (api_version >= VK_API_VERSION_1_2) {
      dev->GetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(
         handle, "vkGetSemaphoreCounterValue");
      dev->WaitSemaphores =
         (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(handle, "vkWaitSemaphores");
      dev->SignalSemaphore =
         (PFN_vkSignalSemaphore)vkGetDeviceProcAddr(handle, "vkSignalSemaphore");
      dev->GetDeviceMemoryOpaqueCaptureAddress =
         (PFN_vkGetDeviceMemoryOpaqueCaptureAddress)vkGetDeviceProcAddr(
            handle, "vkGetDeviceMemoryOpaqueCaptureAddress");
      dev->GetBufferOpaqueCaptureAddress =
         (PFN_vkGetBufferOpaqueCaptureAddress)vkGetDeviceProcAddr(
            handle, "vkGetBufferOpaqueCaptureAddress");
      dev->GetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(
         handle, "vkGetBufferDeviceAddress");
      dev->ResetQueryPool =
         (PFN_vkResetQueryPool)vkGetDeviceProcAddr(handle, "vkResetQueryPool");
      dev->CreateRenderPass2 =
         (PFN_vkCreateRenderPass2)vkGetDeviceProcAddr(handle, "vkCreateRenderPass2");
      dev->CmdBeginRenderPass2 =
         (PFN_vkCmdBeginRenderPass2)vkGetDeviceProcAddr(handle, "vkCmdBeginRenderPass2");
      dev->CmdNextSubpass2 =
         (PFN_vkCmdNextSubpass2)vkGetDeviceProcAddr(handle, "vkCmdNextSubpass2");
      dev->CmdEndRenderPass2 =
         (PFN_vkCmdEndRenderPass2)vkGetDeviceProcAddr(handle, "vkCmdEndRenderPass2");
      dev->CmdDrawIndirectCount = (PFN_vkCmdDrawIndirectCount)vkGetDeviceProcAddr(
         handle, "vkCmdDrawIndirectCount");
      dev->CmdDrawIndexedIndirectCount =
         (PFN_vkCmdDrawIndexedIndirectCount)vkGetDeviceProcAddr(
            handle, "vkCmdDrawIndexedIndirectCount");
   } else {
      dev->GetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(
         handle, "vkGetSemaphoreCounterValueKHR");
      dev->WaitSemaphores =
         (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(handle, "vkWaitSemaphoresKHR");
      dev->SignalSemaphore =
         (PFN_vkSignalSemaphore)vkGetDeviceProcAddr(handle, "vkSignalSemaphoreKHR");
      dev->GetDeviceMemoryOpaqueCaptureAddress =
         (PFN_vkGetDeviceMemoryOpaqueCaptureAddress)vkGetDeviceProcAddr(
            handle, "vkGetDeviceMemoryOpaqueCaptureAddressKHR");
      dev->GetBufferOpaqueCaptureAddress =
         (PFN_vkGetBufferOpaqueCaptureAddress)vkGetDeviceProcAddr(
            handle, "vkGetBufferOpaqueCaptureAddressKHR");
      dev->GetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress)vkGetDeviceProcAddr(
         handle, "vkGetBufferDeviceAddressKHR");
      dev->ResetQueryPool =
         (PFN_vkResetQueryPool)vkGetDeviceProcAddr(handle, "vkResetQueryPoolEXT");
      dev->CreateRenderPass2 =
         (PFN_vkCreateRenderPass2)vkGetDeviceProcAddr(handle, "vkCreateRenderPass2KHR");
      dev->CmdBeginRenderPass2 = (PFN_vkCmdBeginRenderPass2)vkGetDeviceProcAddr(
         handle, "vkCmdBeginRenderPass2KHR");
      dev->CmdNextSubpass2 =
         (PFN_vkCmdNextSubpass2)vkGetDeviceProcAddr(handle, "vkCmdNextSubpass2KHR");
      dev->CmdEndRenderPass2 =
         (PFN_vkCmdEndRenderPass2)vkGetDeviceProcAddr(handle, "vkCmdEndRenderPass2KHR");
      dev->CmdDrawIndirectCount = (PFN_vkCmdDrawIndirectCount)vkGetDeviceProcAddr(
         handle, "vkCmdDrawIndirectCountKHR");
      dev->CmdDrawIndexedIndirectCount =
         (PFN_vkCmdDrawIndexedIndirectCount)vkGetDeviceProcAddr(
            handle, "vkCmdDrawIndexedIndirectCountKHR");
   }

   if (api_version >= VK_API_VERSION_1_3) {
      dev->cmd_bind_vertex_buffers_2 = (PFN_vkCmdBindVertexBuffers2)vkGetDeviceProcAddr(
         handle, "vkCmdBindVertexBuffers2");
      dev->cmd_set_cull_mode =
         (PFN_vkCmdSetCullMode)vkGetDeviceProcAddr(handle, "vkCmdSetCullMode");
      dev->cmd_set_depth_bounds_test_enable =
         (PFN_vkCmdSetDepthBoundsTestEnable)vkGetDeviceProcAddr(
            handle, "vkCmdSetDepthBoundsTestEnable");
      dev->cmd_set_depth_compare_op = (PFN_vkCmdSetDepthCompareOp)vkGetDeviceProcAddr(
         handle, "vkCmdSetDepthCompareOp");
      dev->cmd_set_depth_test_enable = (PFN_vkCmdSetDepthTestEnable)vkGetDeviceProcAddr(
         handle, "vkCmdSetDepthTestEnable");
      dev->cmd_set_depth_write_enable = (PFN_vkCmdSetDepthWriteEnable)vkGetDeviceProcAddr(
         handle, "vkCmdSetDepthWriteEnable");
      dev->cmd_set_front_face =
         (PFN_vkCmdSetFrontFace)vkGetDeviceProcAddr(handle, "vkCmdSetFrontFace");
      dev->cmd_set_primitive_topology =
         (PFN_vkCmdSetPrimitiveTopology)vkGetDeviceProcAddr(handle,
                                                            "vkCmdSetPrimitiveTopology");
      dev->cmd_set_scissor_with_count = (PFN_vkCmdSetScissorWithCount)vkGetDeviceProcAddr(
         handle, "vkCmdSetScissorWithCount");
      dev->cmd_set_stencil_op =
         (PFN_vkCmdSetStencilOp)vkGetDeviceProcAddr(handle, "vkCmdSetStencilOp");
      dev->cmd_set_stencil_test_enable =
         (PFN_vkCmdSetStencilTestEnable)vkGetDeviceProcAddr(handle,
                                                            "vkCmdSetStencilTestEnable");
      dev->cmd_set_viewport_with_count =
         (PFN_vkCmdSetViewportWithCount)vkGetDeviceProcAddr(handle,
                                                            "vkCmdSetViewportWithCount");
   } else {
      dev->cmd_bind_vertex_buffers_2 = (PFN_vkCmdBindVertexBuffers2)vkGetDeviceProcAddr(
         handle, "vkCmdBindVertexBuffers2EXT");
      dev->cmd_set_cull_mode =
         (PFN_vkCmdSetCullMode)vkGetDeviceProcAddr(handle, "vkCmdSetCullModeEXT");
      dev->cmd_set_depth_bounds_test_enable =
         (PFN_vkCmdSetDepthBoundsTestEnable)vkGetDeviceProcAddr(
            handle, "vkCmdSetDepthBoundsTestEnableEXT");
      dev->cmd_set_depth_compare_op = (PFN_vkCmdSetDepthCompareOp)vkGetDeviceProcAddr(
         handle, "vkCmdSetDepthCompareOpEXT");
      dev->cmd_set_depth_test_enable = (PFN_vkCmdSetDepthTestEnable)vkGetDeviceProcAddr(
         handle, "vkCmdSetDepthTestEnableEXT");
      dev->cmd_set_depth_write_enable = (PFN_vkCmdSetDepthWriteEnable)vkGetDeviceProcAddr(
         handle, "vkCmdSetDepthWriteEnableEXT");
      dev->cmd_set_front_face =
         (PFN_vkCmdSetFrontFace)vkGetDeviceProcAddr(handle, "vkCmdSetFrontFaceEXT");
      dev->cmd_set_primitive_topology =
         (PFN_vkCmdSetPrimitiveTopology)vkGetDeviceProcAddr(
            handle, "vkCmdSetPrimitiveTopologyEXT");
      dev->cmd_set_scissor_with_count = (PFN_vkCmdSetScissorWithCount)vkGetDeviceProcAddr(
         handle, "vkCmdSetScissorWithCountEXT");
      dev->cmd_set_stencil_op =
         (PFN_vkCmdSetStencilOp)vkGetDeviceProcAddr(handle, "vkCmdSetStencilOpEXT");
      dev->cmd_set_stencil_test_enable =
         (PFN_vkCmdSetStencilTestEnable)vkGetDeviceProcAddr(
            handle, "vkCmdSetStencilTestEnableEXT");
      dev->cmd_set_viewport_with_count =
         (PFN_vkCmdSetViewportWithCount)vkGetDeviceProcAddr(
            handle, "vkCmdSetViewportWithCountEXT");
   }

   dev->cmd_set_line_stipple =
      (PFN_vkCmdSetLineStippleEXT)vkGetDeviceProcAddr(handle, "vkCmdSetLineStippleEXT");

   dev->cmd_bind_transform_feedback_buffers =
      (PFN_vkCmdBindTransformFeedbackBuffersEXT)vkGetDeviceProcAddr(
         handle, "vkCmdBindTransformFeedbackBuffersEXT");
   dev->cmd_begin_transform_feedback =
      (PFN_vkCmdBeginTransformFeedbackEXT)vkGetDeviceProcAddr(
         handle, "vkCmdBeginTransformFeedbackEXT");
   dev->cmd_end_transform_feedback =
      (PFN_vkCmdEndTransformFeedbackEXT)vkGetDeviceProcAddr(
         handle, "vkCmdEndTransformFeedbackEXT");
   dev->cmd_begin_query_indexed = (PFN_vkCmdBeginQueryIndexedEXT)vkGetDeviceProcAddr(
      handle, "vkCmdBeginQueryIndexedEXT");
   dev->cmd_end_query_indexed =
      (PFN_vkCmdEndQueryIndexedEXT)vkGetDeviceProcAddr(handle, "vkCmdEndQueryIndexedEXT");
   dev->cmd_draw_indirect_byte_count =
      (PFN_vkCmdDrawIndirectByteCountEXT)vkGetDeviceProcAddr(
         handle, "vkCmdDrawIndirectByteCountEXT");

   dev->get_image_drm_format_modifier_properties =
      (PFN_vkGetImageDrmFormatModifierPropertiesEXT)vkGetDeviceProcAddr(
         handle, "vkGetImageDrmFormatModifierPropertiesEXT");

   dev->get_fence_fd =
      (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(handle, "vkGetFenceFdKHR");
   dev->get_memory_fd =
      (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(handle, "vkGetMemoryFdKHR");
   dev->get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
      handle, "vkGetMemoryFdPropertiesKHR");
}

static void
vkr_device_init_proc_table(struct vkr_device *dev,
                           uint32_t api_version,
                           const char *const *exts,
                           uint32_t count)
{
   struct vn_info_extension_table ext_table;
   vkr_extension_table_init(&ext_table, exts, count);

   vn_util_init_device_proc_table(dev->base.handle.device, api_version, &ext_table,
                                  &dev->proc_table);
}

static void
vkr_dispatch_vkCreateDevice(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkCreateDevice *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_physical_device *physical_dev =
      vkr_physical_device_from_handle(args->physicalDevice);

   /* append extensions for our own use */
   const char **exts = NULL;
   uint32_t ext_count = args->pCreateInfo->enabledExtensionCount;
   ext_count += physical_dev->KHR_external_memory_fd;
   ext_count += physical_dev->EXT_external_memory_dma_buf;
   ext_count += physical_dev->KHR_external_fence_fd;
   if (ext_count > args->pCreateInfo->enabledExtensionCount) {
      exts = malloc(sizeof(*exts) * ext_count);
      if (!exts) {
         args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
         return;
      }
      for (uint32_t i = 0; i < args->pCreateInfo->enabledExtensionCount; i++)
         exts[i] = args->pCreateInfo->ppEnabledExtensionNames[i];

      ext_count = args->pCreateInfo->enabledExtensionCount;
      if (physical_dev->KHR_external_memory_fd)
         exts[ext_count++] = "VK_KHR_external_memory_fd";
      if (physical_dev->EXT_external_memory_dma_buf)
         exts[ext_count++] = "VK_EXT_external_memory_dma_buf";
      if (physical_dev->KHR_external_fence_fd)
         exts[ext_count++] = "VK_KHR_external_fence_fd";

      ((VkDeviceCreateInfo *)args->pCreateInfo)->ppEnabledExtensionNames = exts;
      ((VkDeviceCreateInfo *)args->pCreateInfo)->enabledExtensionCount = ext_count;
   }

   struct vkr_device *dev =
      vkr_context_alloc_object(ctx, sizeof(*dev), VK_OBJECT_TYPE_DEVICE, args->pDevice);
   if (!dev) {
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
      free(exts);
      return;
   }

   vn_replace_vkCreateDevice_args_handle(args);
   args->ret = vkCreateDevice(args->physicalDevice, args->pCreateInfo, NULL,
                              &dev->base.handle.device);
   if (args->ret != VK_SUCCESS) {
      free(exts);
      free(dev);
      return;
   }

   dev->physical_device = physical_dev;

   vkr_device_init_proc_table(dev, physical_dev->api_version, exts, ext_count);

   free(exts);

   args->ret = vkr_device_create_queues(ctx, dev, args->pCreateInfo->queueCreateInfoCount,
                                        args->pCreateInfo->pQueueCreateInfos);
   if (args->ret != VK_SUCCESS) {
      vkDestroyDevice(dev->base.handle.device, NULL);
      free(dev);
      return;
   }

   vkr_device_init_entry_points(dev, physical_dev->api_version);

   mtx_init(&dev->free_sync_mutex, mtx_plain);
   list_inithead(&dev->free_syncs);

   list_inithead(&dev->objects);

   list_add(&dev->base.track_head, &physical_dev->devices);

   vkr_context_add_object(ctx, &dev->base);
}

static void
vkr_device_object_destroy(struct vkr_context *ctx,
                          struct vkr_device *dev,
                          struct vkr_object *obj)
{
   VkDevice device = dev->base.handle.device;

   assert(vkr_device_should_track_object(obj));

   switch (obj->type) {
   case VK_OBJECT_TYPE_SEMAPHORE:
      vkDestroySemaphore(device, obj->handle.semaphore, NULL);
      break;
   case VK_OBJECT_TYPE_FENCE:
      vkDestroyFence(device, obj->handle.fence, NULL);
      break;
   case VK_OBJECT_TYPE_DEVICE_MEMORY:
      vkFreeMemory(device, obj->handle.device_memory, NULL);
      vkr_device_memory_release((struct vkr_device_memory *)obj);
      break;
   case VK_OBJECT_TYPE_BUFFER:
      vkDestroyBuffer(device, obj->handle.buffer, NULL);
      break;
   case VK_OBJECT_TYPE_IMAGE:
      vkDestroyImage(device, obj->handle.image, NULL);
      break;
   case VK_OBJECT_TYPE_EVENT:
      vkDestroyEvent(device, obj->handle.event, NULL);
      break;
   case VK_OBJECT_TYPE_QUERY_POOL:
      vkDestroyQueryPool(device, obj->handle.query_pool, NULL);
      break;
   case VK_OBJECT_TYPE_BUFFER_VIEW:
      vkDestroyBufferView(device, obj->handle.buffer_view, NULL);
      break;
   case VK_OBJECT_TYPE_IMAGE_VIEW:
      vkDestroyImageView(device, obj->handle.image_view, NULL);
      break;
   case VK_OBJECT_TYPE_SHADER_MODULE:
      vkDestroyShaderModule(device, obj->handle.shader_module, NULL);
      break;
   case VK_OBJECT_TYPE_PIPELINE_CACHE:
      vkDestroyPipelineCache(device, obj->handle.pipeline_cache, NULL);
      break;
   case VK_OBJECT_TYPE_PIPELINE_LAYOUT:
      vkDestroyPipelineLayout(device, obj->handle.pipeline_layout, NULL);
      break;
   case VK_OBJECT_TYPE_RENDER_PASS:
      vkDestroyRenderPass(device, obj->handle.render_pass, NULL);
      break;
   case VK_OBJECT_TYPE_PIPELINE:
      vkDestroyPipeline(device, obj->handle.pipeline, NULL);
      break;
   case VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT:
      vkDestroyDescriptorSetLayout(device, obj->handle.descriptor_set_layout, NULL);
      break;
   case VK_OBJECT_TYPE_SAMPLER:
      vkDestroySampler(device, obj->handle.sampler, NULL);
      break;
   case VK_OBJECT_TYPE_DESCRIPTOR_POOL: {
      /* Destroying VkDescriptorPool frees all VkDescriptorSet allocated inside. */
      vkDestroyDescriptorPool(device, obj->handle.descriptor_pool, NULL);
      vkr_descriptor_pool_release(ctx, (struct vkr_descriptor_pool *)obj);
      break;
   }
   case VK_OBJECT_TYPE_FRAMEBUFFER:
      vkDestroyFramebuffer(device, obj->handle.framebuffer, NULL);
      break;
   case VK_OBJECT_TYPE_COMMAND_POOL: {
      /* Destroying VkCommandPool frees all VkCommandBuffer allocated inside. */
      vkDestroyCommandPool(device, obj->handle.command_pool, NULL);
      vkr_command_pool_release(ctx, (struct vkr_command_pool *)obj);
      break;
   }
   case VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION:
      vkDestroySamplerYcbcrConversion(device, obj->handle.sampler_ycbcr_conversion, NULL);
      break;
   case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE:
      vkDestroyDescriptorUpdateTemplate(device, obj->handle.descriptor_update_template,
                                        NULL);
      break;
   default:
      vkr_log("Unhandled vkr_object(%p) with VkObjectType(%u)", obj, (uint32_t)obj->type);
      assert(false);
      break;
   };

   vkr_device_remove_object(ctx, dev, obj);
}

void
vkr_device_destroy(struct vkr_context *ctx, struct vkr_device *dev)
{
   VkDevice device = dev->base.handle.device;

   if (!LIST_IS_EMPTY(&dev->objects))
      vkr_log("destroying device with valid objects");

   VkResult result = vkDeviceWaitIdle(device);
   if (result != VK_SUCCESS)
      vkr_log("vkDeviceWaitIdle(%p) failed(%d)", dev, (int32_t)result);

   if (!LIST_IS_EMPTY(&dev->objects)) {
      struct vkr_object *obj, *obj_tmp;
      LIST_FOR_EACH_ENTRY_SAFE (obj, obj_tmp, &dev->objects, track_head)
         vkr_device_object_destroy(ctx, dev, obj);
   }

   struct vkr_queue *queue, *queue_tmp;
   LIST_FOR_EACH_ENTRY_SAFE (queue, queue_tmp, &dev->queues, base.track_head)
      vkr_queue_destroy(ctx, queue);

   struct vkr_queue_sync *sync, *sync_tmp;
   LIST_FOR_EACH_ENTRY_SAFE (sync, sync_tmp, &dev->free_syncs, head) {
      vkDestroyFence(dev->base.handle.device, sync->fence, NULL);
      free(sync);
   }

   mtx_destroy(&dev->free_sync_mutex);

   vkDestroyDevice(device, NULL);

   list_del(&dev->base.track_head);

   vkr_context_remove_object(ctx, &dev->base);
}

static void
vkr_dispatch_vkDestroyDevice(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkDestroyDevice *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = vkr_device_from_handle(args->device);
   /* this never happens */
   if (!dev)
      return;

   vkr_device_destroy(ctx, dev);
}

static void
vkr_dispatch_vkGetDeviceGroupPeerMemoryFeatures(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceGroupPeerMemoryFeatures *args)
{
   vn_replace_vkGetDeviceGroupPeerMemoryFeatures_args_handle(args);
   vkGetDeviceGroupPeerMemoryFeatures(args->device, args->heapIndex,
                                      args->localDeviceIndex, args->remoteDeviceIndex,
                                      args->pPeerMemoryFeatures);
}

static void
vkr_dispatch_vkDeviceWaitIdle(struct vn_dispatch_context *dispatch,
                              UNUSED struct vn_command_vkDeviceWaitIdle *args)
{
   struct vkr_context *ctx = dispatch->data;
   /* no blocking call */
   vkr_cs_decoder_set_fatal(&ctx->decoder);
}

void
vkr_context_init_device_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateDevice = vkr_dispatch_vkCreateDevice;
   dispatch->dispatch_vkDestroyDevice = vkr_dispatch_vkDestroyDevice;
   dispatch->dispatch_vkGetDeviceProcAddr = NULL;
   dispatch->dispatch_vkGetDeviceGroupPeerMemoryFeatures =
      vkr_dispatch_vkGetDeviceGroupPeerMemoryFeatures;
   dispatch->dispatch_vkDeviceWaitIdle = vkr_dispatch_vkDeviceWaitIdle;
}
