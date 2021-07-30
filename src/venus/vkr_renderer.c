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
vkr_dispatch_vkEnumerateInstanceVersion(struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkEnumerateInstanceVersion *args)
{
   struct vkr_context *ctx = dispatch->data;

   if (!args->pApiVersion) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkEnumerateInstanceVersion_args_handle(args);
   args->ret = vkEnumerateInstanceVersion(args->pApiVersion);
}

static void
vkr_dispatch_vkEnumerateInstanceExtensionProperties(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkEnumerateInstanceExtensionProperties *args)
{
   struct vkr_context *ctx = dispatch->data;

   VkExtensionProperties private_extensions[] = {
      {
         .extensionName = "VK_EXT_command_serialization",
      },
      {
         .extensionName = "VK_MESA_venus_protocol",
      },
   };

   if (!args->pPropertyCount) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   if (!args->pProperties) {
      *args->pPropertyCount = ARRAY_SIZE(private_extensions);
      args->ret = VK_SUCCESS;
      return;
   }

   for (uint32_t i = 0; i < ARRAY_SIZE(private_extensions); i++) {
      VkExtensionProperties *props = &private_extensions[i];
      props->specVersion = vn_info_extension_spec_version(props->extensionName);
   }

   const uint32_t count = MIN2(*args->pPropertyCount, ARRAY_SIZE(private_extensions));
   memcpy(args->pProperties, private_extensions, sizeof(*args->pProperties) * count);
   *args->pPropertyCount = count;
   args->ret = count == ARRAY_SIZE(private_extensions) ? VK_SUCCESS : VK_INCOMPLETE;
}

static VkBool32
vkr_validation_callback(UNUSED VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
                        UNUSED VkDebugUtilsMessageTypeFlagsEXT messageTypes,
                        const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
                        void *pUserData)
{
   struct vkr_context *ctx = pUserData;

   vrend_printf("%s\n", pCallbackData->pMessage);

   if (!ctx->validate_fatal)
      return false;

   vkr_cs_decoder_set_fatal(&ctx->decoder);

   /* The spec says we "should" return false, because the meaning of true is
    * layer-defined and is reserved for layer development.  And we know that,
    * for VK_LAYER_KHRONOS_validation, the return value indicates whether the
    * call should be skipped.  Let's do it for now and seek advices.
    */
   return true;
}

static void
vkr_dispatch_vkCreateInstance(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkCreateInstance *args)
{
   struct vkr_context *ctx = dispatch->data;

   if (ctx->instance) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   if (!args->pCreateInfo) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   if (args->pCreateInfo->enabledLayerCount) {
      args->ret = VK_ERROR_LAYER_NOT_PRESENT;
      return;
   }

   if (args->pCreateInfo->enabledExtensionCount) {
      args->ret = VK_ERROR_EXTENSION_NOT_PRESENT;
      return;
   }

   uint32_t instance_version;
   args->ret = vkEnumerateInstanceVersion(&instance_version);
   if (args->ret != VK_SUCCESS)
      return;

   /* require Vulkan 1.1 */
   if (instance_version < VK_API_VERSION_1_1) {
      args->ret = VK_ERROR_INITIALIZATION_FAILED;
      return;
   }

   VkInstanceCreateInfo *create_info = (VkInstanceCreateInfo *)args->pCreateInfo;
   const char *layer_names[8];
   const char *ext_names[8];
   uint32_t layer_count = 0;
   uint32_t ext_count = 0;

   /* TODO enable more validation features */
   const VkValidationFeatureDisableEXT validation_feature_disables_on[] = {
      VK_VALIDATION_FEATURE_DISABLE_THREAD_SAFETY_EXT,
      VK_VALIDATION_FEATURE_DISABLE_SHADERS_EXT,
      VK_VALIDATION_FEATURE_DISABLE_OBJECT_LIFETIMES_EXT,
      VK_VALIDATION_FEATURE_DISABLE_CORE_CHECKS_EXT,
      VK_VALIDATION_FEATURE_DISABLE_UNIQUE_HANDLES_EXT,
   };
   /* we are single-threaded */
   const VkValidationFeatureDisableEXT validation_feature_disables_full[] = {
      VK_VALIDATION_FEATURE_DISABLE_THREAD_SAFETY_EXT,
   };
   VkValidationFeaturesEXT validation_features;
   VkDebugUtilsMessengerCreateInfoEXT messenger_create_info;
   if (ctx->validate_level != VKR_CONTEXT_VALIDATE_NONE) {
      /* let vkCreateInstance return VK_ERROR_LAYER_NOT_PRESENT or
       * VK_ERROR_EXTENSION_NOT_PRESENT when the layer or extensions are
       * missing
       */
      layer_names[layer_count++] = "VK_LAYER_KHRONOS_validation";
      ext_names[ext_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
      ext_names[ext_count++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;

      validation_features = (const VkValidationFeaturesEXT){
         .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
         .pNext = create_info->pNext,
      };
      if (ctx->validate_level == VKR_CONTEXT_VALIDATE_ON) {
         validation_features.disabledValidationFeatureCount =
            ARRAY_SIZE(validation_feature_disables_on);
         validation_features.pDisabledValidationFeatures = validation_feature_disables_on;
      } else {
         validation_features.disabledValidationFeatureCount =
            ARRAY_SIZE(validation_feature_disables_full);
         validation_features.pDisabledValidationFeatures =
            validation_feature_disables_full;
      }
      messenger_create_info = (VkDebugUtilsMessengerCreateInfoEXT){
         .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
         .pNext = &validation_features,
         .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
         .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
         .pfnUserCallback = vkr_validation_callback,
         .pUserData = ctx,
      };

      create_info->pNext = &messenger_create_info;
   }

   assert(layer_count <= ARRAY_SIZE(layer_names));
   create_info->enabledLayerCount = layer_count;
   create_info->ppEnabledLayerNames = layer_names;

   assert(ext_count <= ARRAY_SIZE(ext_names));
   create_info->enabledExtensionCount = ext_count;
   create_info->ppEnabledExtensionNames = ext_names;

   /* patch apiVersion */
   VkApplicationInfo app_info = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_1,
   };
   if (create_info->pApplicationInfo) {
      app_info = *create_info->pApplicationInfo;
      if (app_info.apiVersion < VK_API_VERSION_1_1)
         app_info.apiVersion = VK_API_VERSION_1_1;
   }
   create_info->pApplicationInfo = &app_info;

   struct vkr_instance *instance = calloc(1, sizeof(*instance));
   if (!instance) {
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
      return;
   }

   instance->base.type = VK_OBJECT_TYPE_INSTANCE;
   instance->base.id =
      vkr_cs_handle_load_id((const void **)args->pInstance, instance->base.type);
   instance->api_version = app_info.apiVersion;

   vn_replace_vkCreateInstance_args_handle(args);
   args->ret = vkCreateInstance(create_info, NULL, &instance->base.handle.instance);
   if (args->ret != VK_SUCCESS) {
      free(instance);
      return;
   }

   instance->get_memory_fd = (PFN_vkGetMemoryFdKHR)vkGetInstanceProcAddr(
      instance->base.handle.instance, "vkGetMemoryFdKHR");
   instance->get_fence_fd = (PFN_vkGetFenceFdKHR)vkGetInstanceProcAddr(
      instance->base.handle.instance, "vkGetFenceFdKHR");

   if (ctx->validate_level != VKR_CONTEXT_VALIDATE_NONE) {
      instance->create_debug_utils_messenger =
         (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance->base.handle.instance, "vkCreateDebugUtilsMessengerEXT");
      instance->destroy_debug_utils_messenger =
         (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            instance->base.handle.instance, "vkDestroyDebugUtilsMessengerEXT");

      messenger_create_info.pNext = NULL;
      args->ret = instance->create_debug_utils_messenger(instance->base.handle.instance,
                                                         &messenger_create_info, NULL,
                                                         &instance->validation_messenger);
      if (args->ret != VK_SUCCESS) {
         vkDestroyInstance(instance->base.handle.instance, NULL);
         free(instance);
         return;
      }
   }

   util_hash_table_set_u64(ctx->object_table, instance->base.id, instance);

   ctx->instance = instance;
}

static void
vkr_device_destroy(struct vkr_context *ctx, struct vkr_device *dev);

static void
vkr_physical_device_destroy(struct vkr_context *ctx,
                            struct vkr_physical_device *physical_dev)
{
   struct vkr_device *dev, *tmp;
   LIST_FOR_EACH_ENTRY_SAFE (dev, tmp, &physical_dev->devices, base.track_head)
      vkr_device_destroy(ctx, dev);

   free(physical_dev->extensions);

   util_hash_table_remove_u64(ctx->object_table, physical_dev->base.id);
}

static void
vkr_instance_destroy(struct vkr_context *ctx, struct vkr_instance *instance)
{
   for (uint32_t i = 0; i < instance->physical_device_count; i++) {
      struct vkr_physical_device *physical_dev = instance->physical_devices[i];
      if (!physical_dev)
         break;

      vkr_physical_device_destroy(ctx, physical_dev);
   }

   if (ctx->validate_level != VKR_CONTEXT_VALIDATE_NONE) {
      instance->destroy_debug_utils_messenger(instance->base.handle.instance,
                                              instance->validation_messenger, NULL);
   }

   vkDestroyInstance(instance->base.handle.instance, NULL);

   free(instance->physical_device_handles);
   free(instance->physical_devices);

   util_hash_table_remove_u64(ctx->object_table, instance->base.id);
}

static void
vkr_dispatch_vkDestroyInstance(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkDestroyInstance *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_instance *instance = (struct vkr_instance *)args->instance;

   if (ctx->instance != instance) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vkr_instance_destroy(ctx, instance);

   ctx->instance = NULL;
}

static VkResult
vkr_instance_enumerate_physical_devices(struct vkr_instance *instance)
{
   if (instance->physical_device_count)
      return VK_SUCCESS;

   uint32_t count;
   VkResult result =
      vkEnumeratePhysicalDevices(instance->base.handle.instance, &count, NULL);
   if (result != VK_SUCCESS)
      return result;

   VkPhysicalDevice *handles = calloc(count, sizeof(*handles));
   struct vkr_physical_device **physical_devs = calloc(count, sizeof(*physical_devs));
   if (!handles || !physical_devs) {
      free(physical_devs);
      free(handles);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   result = vkEnumeratePhysicalDevices(instance->base.handle.instance, &count, handles);
   if (result != VK_SUCCESS) {
      free(physical_devs);
      free(handles);
      return result;
   }

   instance->physical_device_count = count;
   instance->physical_device_handles = handles;
   instance->physical_devices = physical_devs;

   return VK_SUCCESS;
}

static struct vkr_physical_device *
vkr_instance_lookup_physical_device(struct vkr_instance *instance,
                                    VkPhysicalDevice handle)
{
   for (uint32_t i = 0; i < instance->physical_device_count; i++) {
      /* XXX this assumes VkPhysicalDevice handles are unique */
      if (instance->physical_device_handles[i] == handle)
         return instance->physical_devices[i];
   }
   return NULL;
}

static void
vkr_physical_device_init_memory_properties(struct vkr_physical_device *physical_dev)
{
   VkPhysicalDevice handle = physical_dev->base.handle.physical_device;
   vkGetPhysicalDeviceMemoryProperties(handle, &physical_dev->memory_properties);
}

static void
vkr_physical_device_init_extensions(struct vkr_physical_device *physical_dev,
                                    struct vkr_instance *instance)
{
   VkPhysicalDevice handle = physical_dev->base.handle.physical_device;

   VkExtensionProperties *exts;
   uint32_t count;
   VkResult result = vkEnumerateDeviceExtensionProperties(handle, NULL, &count, NULL);
   if (result != VK_SUCCESS)
      return;

   exts = malloc(sizeof(*exts) * count);
   if (!exts)
      return;

   result = vkEnumerateDeviceExtensionProperties(handle, NULL, &count, exts);
   if (result != VK_SUCCESS) {
      free(exts);
      return;
   }

   uint32_t advertised_count = 0;
   for (uint32_t i = 0; i < count; i++) {
      VkExtensionProperties *props = &exts[i];

      if (!strcmp(props->extensionName, "VK_KHR_external_memory_fd"))
         physical_dev->KHR_external_memory_fd = true;
      else if (!strcmp(props->extensionName, "VK_EXT_external_memory_dma_buf"))
         physical_dev->EXT_external_memory_dma_buf = true;
      else if (!strcmp(props->extensionName, "VK_KHR_external_fence_fd"))
         physical_dev->KHR_external_fence_fd = true;

      const uint32_t spec_ver = vn_info_extension_spec_version(props->extensionName);
      if (spec_ver) {
         if (props->specVersion > spec_ver)
            props->specVersion = spec_ver;
         exts[advertised_count++] = exts[i];
      }
   }

   if (physical_dev->KHR_external_fence_fd) {
      const VkPhysicalDeviceExternalFenceInfo fence_info = {
         .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      VkExternalFenceProperties fence_props = {
         .sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES,
      };
      PFN_vkGetPhysicalDeviceExternalFenceProperties get_fence_props =
         (PFN_vkGetPhysicalDeviceExternalFenceProperties)vkGetInstanceProcAddr(
            instance->base.handle.instance, "vkGetPhysicalDeviceExternalFenceProperties");
      get_fence_props(handle, &fence_info, &fence_props);

      if (!(fence_props.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT))
         physical_dev->KHR_external_fence_fd = false;
   }

   physical_dev->extensions = exts;
   physical_dev->extension_count = advertised_count;
}

static void
vkr_physical_device_init_properties(struct vkr_physical_device *physical_dev)
{
   VkPhysicalDevice handle = physical_dev->base.handle.physical_device;
   vkGetPhysicalDeviceProperties(handle, &physical_dev->properties);

   VkPhysicalDeviceProperties *props = &physical_dev->properties;
   props->driverVersion = 0;

   /* TODO lie about props->pipelineCacheUUID and patch cache header */
}

static void
vkr_dispatch_vkEnumeratePhysicalDevices(struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkEnumeratePhysicalDevices *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_instance *instance = (struct vkr_instance *)args->instance;
   if (instance != ctx->instance) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   args->ret = vkr_instance_enumerate_physical_devices(instance);
   if (args->ret != VK_SUCCESS)
      return;

   uint32_t count = instance->physical_device_count;
   if (!args->pPhysicalDevices) {
      *args->pPhysicalDeviceCount = count;
      args->ret = VK_SUCCESS;
      return;
   }

   if (count > *args->pPhysicalDeviceCount) {
      count = *args->pPhysicalDeviceCount;
      args->ret = VK_INCOMPLETE;
   } else {
      *args->pPhysicalDeviceCount = count;
      args->ret = VK_SUCCESS;
   }

   uint32_t i;
   for (i = 0; i < count; i++) {
      struct vkr_physical_device *physical_dev = instance->physical_devices[i];
      const vkr_object_id id = vkr_cs_handle_load_id(
         (const void **)&args->pPhysicalDevices[i], VK_OBJECT_TYPE_PHYSICAL_DEVICE);

      if (physical_dev) {
         if (physical_dev->base.id != id) {
            vkr_cs_decoder_set_fatal(&ctx->decoder);
            break;
         }
         continue;
      }

      physical_dev = calloc(1, sizeof(*physical_dev));
      if (!physical_dev) {
         args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
         break;
      }

      physical_dev->base.type = VK_OBJECT_TYPE_PHYSICAL_DEVICE;
      physical_dev->base.id = id;
      physical_dev->base.handle.physical_device = instance->physical_device_handles[i];

      vkr_physical_device_init_properties(physical_dev);
      physical_dev->api_version =
         MIN2(physical_dev->properties.apiVersion, instance->api_version);
      vkr_physical_device_init_extensions(physical_dev, instance);
      vkr_physical_device_init_memory_properties(physical_dev);

      list_inithead(&physical_dev->devices);

      instance->physical_devices[i] = physical_dev;

      util_hash_table_set_u64(ctx->object_table, physical_dev->base.id, physical_dev);
   }
   /* remove all physical devices on errors */
   if (i < count) {
      for (i = 0; i < instance->physical_device_count; i++) {
         struct vkr_physical_device *physical_dev = instance->physical_devices[i];
         if (!physical_dev)
            break;
         free(physical_dev->extensions);
         util_hash_table_remove_u64(ctx->object_table, physical_dev->base.id);
         instance->physical_devices[i] = NULL;
      }
   }
}

static void
vkr_dispatch_vkEnumeratePhysicalDeviceGroups(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkEnumeratePhysicalDeviceGroups *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_instance *instance = (struct vkr_instance *)args->instance;
   if (instance != ctx->instance) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   args->ret = vkr_instance_enumerate_physical_devices(instance);
   if (args->ret != VK_SUCCESS)
      return;

   VkPhysicalDeviceGroupProperties *orig_props = args->pPhysicalDeviceGroupProperties;
   if (orig_props) {
      args->pPhysicalDeviceGroupProperties =
         malloc(sizeof(*orig_props) * *args->pPhysicalDeviceGroupCount);
      if (!args->pPhysicalDeviceGroupProperties) {
         args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
         return;
      }
   }

   vn_replace_vkEnumeratePhysicalDeviceGroups_args_handle(args);
   args->ret =
      vkEnumeratePhysicalDeviceGroups(args->instance, args->pPhysicalDeviceGroupCount,
                                      args->pPhysicalDeviceGroupProperties);
   if (args->ret != VK_SUCCESS)
      return;

   if (!orig_props)
      return;

   /* XXX this assumes vkEnumeratePhysicalDevices is called first */
   /* replace VkPhysicalDevice handles by object ids */
   for (uint32_t i = 0; i < *args->pPhysicalDeviceGroupCount; i++) {
      const VkPhysicalDeviceGroupProperties *props =
         &args->pPhysicalDeviceGroupProperties[i];
      VkPhysicalDeviceGroupProperties *out = &orig_props[i];

      out->physicalDeviceCount = props->physicalDeviceCount;
      out->subsetAllocation = props->subsetAllocation;
      for (uint32_t j = 0; j < props->physicalDeviceCount; j++) {
         const struct vkr_physical_device *physical_dev =
            vkr_instance_lookup_physical_device(instance, props->physicalDevices[j]);
         vkr_cs_handle_store_id((void **)&out->physicalDevices[j], physical_dev->base.id,
                                VK_OBJECT_TYPE_PHYSICAL_DEVICE);
      }
   }

   free(args->pPhysicalDeviceGroupProperties);
   args->pPhysicalDeviceGroupProperties = orig_props;
}

static void
vkr_dispatch_vkEnumerateDeviceExtensionProperties(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkEnumerateDeviceExtensionProperties *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_physical_device *physical_dev =
      (struct vkr_physical_device *)args->physicalDevice;
   if (!physical_dev || physical_dev->base.type != VK_OBJECT_TYPE_PHYSICAL_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }
   if (args->pLayerName) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   if (!args->pProperties) {
      *args->pPropertyCount = physical_dev->extension_count;
      args->ret = VK_SUCCESS;
      return;
   }

   uint32_t count = physical_dev->extension_count;
   if (count > *args->pPropertyCount) {
      count = *args->pPropertyCount;
      args->ret = VK_INCOMPLETE;
   } else {
      *args->pPropertyCount = count;
      args->ret = VK_SUCCESS;
   }

   memcpy(args->pProperties, physical_dev->extensions,
          sizeof(*args->pProperties) * count);
}

static void
vkr_dispatch_vkGetPhysicalDeviceFeatures(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceFeatures *args)
{
   vn_replace_vkGetPhysicalDeviceFeatures_args_handle(args);
   vkGetPhysicalDeviceFeatures(args->physicalDevice, args->pFeatures);
}

static void
vkr_dispatch_vkGetPhysicalDeviceProperties(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceProperties *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_physical_device *physical_dev =
      (struct vkr_physical_device *)args->physicalDevice;
   if (!physical_dev || physical_dev->base.type != VK_OBJECT_TYPE_PHYSICAL_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   *args->pProperties = physical_dev->properties;
}

static void
vkr_dispatch_vkGetPhysicalDeviceQueueFamilyProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceQueueFamilyProperties *args)
{
   vn_replace_vkGetPhysicalDeviceQueueFamilyProperties_args_handle(args);
   vkGetPhysicalDeviceQueueFamilyProperties(args->physicalDevice,
                                            args->pQueueFamilyPropertyCount,
                                            args->pQueueFamilyProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceMemoryProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceMemoryProperties *args)
{
   /* TODO lie about this */
   vn_replace_vkGetPhysicalDeviceMemoryProperties_args_handle(args);
   vkGetPhysicalDeviceMemoryProperties(args->physicalDevice, args->pMemoryProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceFormatProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceFormatProperties *args)
{
   vn_replace_vkGetPhysicalDeviceFormatProperties_args_handle(args);
   vkGetPhysicalDeviceFormatProperties(args->physicalDevice, args->format,
                                       args->pFormatProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceImageFormatProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceImageFormatProperties *args)
{
   vn_replace_vkGetPhysicalDeviceImageFormatProperties_args_handle(args);
   args->ret = vkGetPhysicalDeviceImageFormatProperties(
      args->physicalDevice, args->format, args->type, args->tiling, args->usage,
      args->flags, args->pImageFormatProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceSparseImageFormatProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceSparseImageFormatProperties *args)
{
   vn_replace_vkGetPhysicalDeviceSparseImageFormatProperties_args_handle(args);
   vkGetPhysicalDeviceSparseImageFormatProperties(
      args->physicalDevice, args->format, args->type, args->samples, args->usage,
      args->tiling, args->pPropertyCount, args->pProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceFeatures2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceFeatures2 *args)
{
   vn_replace_vkGetPhysicalDeviceFeatures2_args_handle(args);
   vkGetPhysicalDeviceFeatures2(args->physicalDevice, args->pFeatures);
}

static void
vkr_dispatch_vkGetPhysicalDeviceProperties2(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceProperties2 *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_physical_device *physical_dev =
      (struct vkr_physical_device *)args->physicalDevice;
   if (!physical_dev || physical_dev->base.type != VK_OBJECT_TYPE_PHYSICAL_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkGetPhysicalDeviceProperties2_args_handle(args);
   vkGetPhysicalDeviceProperties2(args->physicalDevice, args->pProperties);

   union {
      VkBaseOutStructure *pnext;
      VkPhysicalDeviceProperties2 *props;
      VkPhysicalDeviceVulkan11Properties *vk11;
      VkPhysicalDeviceVulkan12Properties *vk12;
      VkPhysicalDeviceIDProperties *id;
      VkPhysicalDeviceDriverProperties *driver;
   } u;

   u.pnext = (VkBaseOutStructure *)args->pProperties;
   while (u.pnext) {
      switch (u.pnext->sType) {
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2:
         u.props->properties = physical_dev->properties;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES:
         memset(u.vk11->deviceUUID, 0, sizeof(u.vk11->deviceUUID));
         memset(u.vk11->driverUUID, 0, sizeof(u.vk11->driverUUID));
         memset(u.vk11->deviceLUID, 0, sizeof(u.vk11->deviceLUID));
         u.vk11->deviceNodeMask = 0;
         u.vk11->deviceLUIDValid = false;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES:
         u.vk12->driverID = 0;
         memset(u.vk12->driverName, 0, sizeof(u.vk12->driverName));
         memset(u.vk12->driverInfo, 0, sizeof(u.vk12->driverInfo));
         memset(&u.vk12->conformanceVersion, 0, sizeof(u.vk12->conformanceVersion));
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES:
         memset(u.id->deviceUUID, 0, sizeof(u.id->deviceUUID));
         memset(u.id->driverUUID, 0, sizeof(u.id->driverUUID));
         memset(u.id->deviceLUID, 0, sizeof(u.id->deviceLUID));
         u.id->deviceNodeMask = 0;
         u.id->deviceLUIDValid = false;
         break;
      case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES:
         u.driver->driverID = 0;
         memset(u.driver->driverName, 0, sizeof(u.driver->driverName));
         memset(u.driver->driverInfo, 0, sizeof(u.driver->driverInfo));
         memset(&u.driver->conformanceVersion, 0, sizeof(u.driver->conformanceVersion));
         break;
      default:
         break;
      }

      u.pnext = u.pnext->pNext;
   }
}

static void
vkr_dispatch_vkGetPhysicalDeviceQueueFamilyProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceQueueFamilyProperties2 *args)
{
   vn_replace_vkGetPhysicalDeviceQueueFamilyProperties2_args_handle(args);
   vkGetPhysicalDeviceQueueFamilyProperties2(args->physicalDevice,
                                             args->pQueueFamilyPropertyCount,
                                             args->pQueueFamilyProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceMemoryProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceMemoryProperties2 *args)
{
   /* TODO lie about this */
   vn_replace_vkGetPhysicalDeviceMemoryProperties2_args_handle(args);
   vkGetPhysicalDeviceMemoryProperties2(args->physicalDevice, args->pMemoryProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceFormatProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceFormatProperties2 *args)
{
   vn_replace_vkGetPhysicalDeviceFormatProperties2_args_handle(args);
   vkGetPhysicalDeviceFormatProperties2(args->physicalDevice, args->format,
                                        args->pFormatProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceImageFormatProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceImageFormatProperties2 *args)
{
   vn_replace_vkGetPhysicalDeviceImageFormatProperties2_args_handle(args);
   args->ret = vkGetPhysicalDeviceImageFormatProperties2(
      args->physicalDevice, args->pImageFormatInfo, args->pImageFormatProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceSparseImageFormatProperties2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceSparseImageFormatProperties2 *args)
{
   vn_replace_vkGetPhysicalDeviceSparseImageFormatProperties2_args_handle(args);
   vkGetPhysicalDeviceSparseImageFormatProperties2(
      args->physicalDevice, args->pFormatInfo, args->pPropertyCount, args->pProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceExternalBufferProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceExternalBufferProperties *args)
{
   vn_replace_vkGetPhysicalDeviceExternalBufferProperties_args_handle(args);
   vkGetPhysicalDeviceExternalBufferProperties(
      args->physicalDevice, args->pExternalBufferInfo, args->pExternalBufferProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceExternalSemaphoreProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceExternalSemaphoreProperties *args)
{
   vn_replace_vkGetPhysicalDeviceExternalSemaphoreProperties_args_handle(args);
   vkGetPhysicalDeviceExternalSemaphoreProperties(args->physicalDevice,
                                                  args->pExternalSemaphoreInfo,
                                                  args->pExternalSemaphoreProperties);
}

static void
vkr_dispatch_vkGetPhysicalDeviceExternalFenceProperties(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetPhysicalDeviceExternalFenceProperties *args)
{
   vn_replace_vkGetPhysicalDeviceExternalFenceProperties_args_handle(args);
   vkGetPhysicalDeviceExternalFenceProperties(
      args->physicalDevice, args->pExternalFenceInfo, args->pExternalFenceProperties);
}

static struct vkr_queue_sync *
vkr_device_alloc_queue_sync(struct vkr_device *dev,
                            uint32_t fence_flags,
                            void *fence_cookie)
{
   struct vkr_queue_sync *sync;

   if (vkr_renderer_flags & VKR_RENDERER_ASYNC_FENCE_CB)
      mtx_lock(&dev->free_sync_mutex);

   if (LIST_IS_EMPTY(&dev->free_syncs)) {
      if (vkr_renderer_flags & VKR_RENDERER_ASYNC_FENCE_CB)
         mtx_unlock(&dev->free_sync_mutex);

      sync = malloc(sizeof(*sync));
      if (!sync)
         return NULL;

      const VkExportFenceCreateInfo export_info = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
         .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      const struct VkFenceCreateInfo create_info = {
         .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
         .pNext = dev->physical_device->KHR_external_fence_fd ? &export_info : NULL,
      };
      VkResult result =
         vkCreateFence(dev->base.handle.device, &create_info, NULL, &sync->fence);
      if (result != VK_SUCCESS) {
         free(sync);
         return NULL;
      }
   } else {
      sync = LIST_ENTRY(struct vkr_queue_sync, dev->free_syncs.next, head);
      list_del(&sync->head);

      if (vkr_renderer_flags & VKR_RENDERER_ASYNC_FENCE_CB)
         mtx_unlock(&dev->free_sync_mutex);

      vkResetFences(dev->base.handle.device, 1, &sync->fence);
   }

   sync->flags = fence_flags;
   sync->fence_cookie = fence_cookie;

   return sync;
}

static void
vkr_device_free_queue_sync(struct vkr_device *dev, struct vkr_queue_sync *sync)
{
   if (vkr_renderer_flags & VKR_RENDERER_ASYNC_FENCE_CB) {
      mtx_lock(&dev->free_sync_mutex);
      list_addtail(&sync->head, &dev->free_syncs);
      mtx_unlock(&dev->free_sync_mutex);
   } else {
      list_addtail(&sync->head, &dev->free_syncs);
   }
}

static void
vkr_queue_retire_syncs(struct vkr_queue *queue,
                       struct list_head *retired_syncs,
                       bool *queue_empty)
{
   struct vkr_device *dev = queue->device;
   struct vkr_queue_sync *sync, *tmp;

   assert(!(vkr_renderer_flags & VKR_RENDERER_ASYNC_FENCE_CB));

   list_inithead(retired_syncs);

   if (vkr_renderer_flags & VKR_RENDERER_THREAD_SYNC) {
      mtx_lock(&queue->mutex);

      LIST_FOR_EACH_ENTRY_SAFE (sync, tmp, &queue->signaled_syncs, head) {
         if (sync->head.next == &queue->signaled_syncs ||
             !(sync->flags & VIRGL_RENDERER_FENCE_FLAG_MERGEABLE))
            list_addtail(&sync->head, retired_syncs);
         else
            vkr_device_free_queue_sync(dev, sync);
      }
      list_inithead(&queue->signaled_syncs);

      *queue_empty = LIST_IS_EMPTY(&queue->pending_syncs);

      mtx_unlock(&queue->mutex);
   } else {
      LIST_FOR_EACH_ENTRY_SAFE (sync, tmp, &queue->pending_syncs, head) {
         VkResult result = vkGetFenceStatus(dev->base.handle.device, sync->fence);
         if (result == VK_NOT_READY)
            break;

         list_del(&sync->head);
         if (sync->head.next == &queue->pending_syncs ||
             !(sync->flags & VIRGL_RENDERER_FENCE_FLAG_MERGEABLE))
            list_addtail(&sync->head, retired_syncs);
         else
            vkr_device_free_queue_sync(dev, sync);
      }

      *queue_empty = LIST_IS_EMPTY(&queue->pending_syncs);
   }
}

static int
vkr_queue_thread(void *arg)
{
   struct vkr_queue *queue = arg;
   struct vkr_context *ctx = queue->context;
   struct vkr_device *dev = queue->device;
   const uint64_t ns_per_sec = 1000000000llu;
   char thread_name[16];

   snprintf(thread_name, ARRAY_SIZE(thread_name), "vkr-queue-%d", ctx->base.ctx_id);
   pipe_thread_setname(thread_name);

   mtx_lock(&queue->mutex);
   while (true) {
      while (LIST_IS_EMPTY(&queue->pending_syncs) && !queue->join)
         cnd_wait(&queue->cond, &queue->mutex);

      if (queue->join)
         break;

      struct vkr_queue_sync *sync =
         LIST_ENTRY(struct vkr_queue_sync, queue->pending_syncs.next, head);

      mtx_unlock(&queue->mutex);

      VkResult result =
         vkWaitForFences(dev->base.handle.device, 1, &sync->fence, false, ns_per_sec * 3);

      mtx_lock(&queue->mutex);

      if (result == VK_TIMEOUT)
         continue;

      list_del(&sync->head);

      if (vkr_renderer_flags & VKR_RENDERER_ASYNC_FENCE_CB) {
         ctx->base.fence_retire(&ctx->base, queue->base.id, sync->fence_cookie);
         vkr_device_free_queue_sync(queue->device, sync);
      } else {
         list_addtail(&sync->head, &queue->signaled_syncs);
         write_eventfd(queue->eventfd, 1);
      }
   }
   mtx_unlock(&queue->mutex);

   return 0;
}

static void
vkr_queue_destroy(struct vkr_context *ctx, struct vkr_queue *queue)
{
   struct vkr_queue_sync *sync, *tmp;

   if (vkr_renderer_flags & VKR_RENDERER_THREAD_SYNC) {
      mtx_lock(&queue->mutex);
      queue->join = true;
      mtx_unlock(&queue->mutex);

      cnd_signal(&queue->cond);
      thrd_join(queue->thread, NULL);

      LIST_FOR_EACH_ENTRY_SAFE (sync, tmp, &queue->signaled_syncs, head)
         vkr_device_free_queue_sync(queue->device, sync);
   } else {
      assert(LIST_IS_EMPTY(&queue->signaled_syncs));
   }

   LIST_FOR_EACH_ENTRY_SAFE (sync, tmp, &queue->pending_syncs, head)
      vkr_device_free_queue_sync(queue->device, sync);

   mtx_destroy(&queue->mutex);
   cnd_destroy(&queue->cond);

   list_del(&queue->head);
   list_del(&queue->busy_head);

   util_hash_table_remove_u64(ctx->object_table, queue->base.id);
}

static struct vkr_queue *
vkr_queue_create(struct vkr_context *ctx,
                 struct vkr_device *dev,
                 vkr_object_id id,
                 VkQueue handle,
                 uint32_t family,
                 uint32_t index)
{
   struct vkr_queue *queue;
   int ret;

   LIST_FOR_EACH_ENTRY (queue, &dev->queues, head) {
      if (queue->family == family && queue->index == index)
         return queue;
   }

   queue = calloc(1, sizeof(*queue));
   if (!queue)
      return NULL;

   queue->base.type = VK_OBJECT_TYPE_QUEUE;
   queue->base.id = id;
   queue->base.handle.queue = handle;

   queue->context = ctx;
   queue->device = dev;
   queue->family = family;
   queue->index = index;

   list_inithead(&queue->pending_syncs);
   list_inithead(&queue->signaled_syncs);

   ret = mtx_init(&queue->mutex, mtx_plain);
   if (ret != thrd_success) {
      free(queue);
      return NULL;
   }
   ret = cnd_init(&queue->cond);
   if (ret != thrd_success) {
      mtx_destroy(&queue->mutex);
      free(queue);
      return NULL;
   }

   if (vkr_renderer_flags & VKR_RENDERER_THREAD_SYNC) {
      ret = thrd_create(&queue->thread, vkr_queue_thread, queue);
      if (ret != thrd_success) {
         mtx_destroy(&queue->mutex);
         cnd_destroy(&queue->cond);
         free(queue);
         return NULL;
      }
      queue->eventfd = ctx->fence_eventfd;
   }

   /* currently queues are not tracked as device objects */
   list_addtail(&queue->head, &dev->queues);
   list_inithead(&queue->busy_head);

   util_hash_table_set_u64(ctx->object_table, queue->base.id, queue);

   return queue;
}

static void
vkr_dispatch_vkCreateDevice(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkCreateDevice *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_physical_device *physical_dev =
      (struct vkr_physical_device *)args->physicalDevice;
   if (!physical_dev || physical_dev->base.type != VK_OBJECT_TYPE_PHYSICAL_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

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

   struct vkr_device *dev = calloc(1, sizeof(*dev));
   if (!dev) {
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
      free(exts);
      return;
   }

   dev->base.type = VK_OBJECT_TYPE_DEVICE;
   dev->base.id = vkr_cs_handle_load_id((const void **)args->pDevice, dev->base.type);

   vn_replace_vkCreateDevice_args_handle(args);
   args->ret = vkCreateDevice(args->physicalDevice, args->pCreateInfo, NULL,
                              &dev->base.handle.device);
   if (args->ret != VK_SUCCESS) {
      free(exts);
      free(dev);
      return;
   }

   free(exts);

   dev->physical_device = physical_dev;

   VkDevice handle = dev->base.handle.device;
   if (physical_dev->api_version >= VK_API_VERSION_1_2) {
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

   dev->get_memory_fd_properties = (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
      handle, "vkGetMemoryFdPropertiesKHR");

   list_inithead(&dev->queues);

   mtx_init(&dev->free_sync_mutex, mtx_plain);
   list_inithead(&dev->free_syncs);

   list_inithead(&dev->objects);

   list_add(&dev->base.track_head, &physical_dev->devices);

   util_hash_table_set_u64(ctx->object_table, dev->base.id, dev);
}

static void
vkr_device_object_destroy(struct vkr_context *ctx,
                          struct vkr_device *dev,
                          struct vkr_object *obj)
{
   VkDevice device = dev->base.handle.device;

   switch (obj->type) {
   case VK_OBJECT_TYPE_SEMAPHORE:
      vkDestroySemaphore(device, obj->handle.semaphore, NULL);
      break;
   case VK_OBJECT_TYPE_FENCE:
      vkDestroyFence(device, obj->handle.fence, NULL);
      break;
   case VK_OBJECT_TYPE_DEVICE_MEMORY:
      vkFreeMemory(device, obj->handle.device_memory, NULL);

      /* remove device memory from exported or attachment list */
      list_del(&((struct vkr_device_memory *)obj)->head);
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
      /* Destroying VkDescriptorPool frees all VkDescriptorSet objects that were allocated
       * from it.
       */
      vkDestroyDescriptorPool(device, obj->handle.descriptor_pool, NULL);

      struct vkr_descriptor_set *set, *tmp;
      LIST_FOR_EACH_ENTRY_SAFE (
         set, tmp, &((struct vkr_descriptor_pool *)obj)->descriptor_sets, head)
         util_hash_table_remove_u64(ctx->object_table, set->base.id);

      break;
   }
   case VK_OBJECT_TYPE_FRAMEBUFFER:
      vkDestroyFramebuffer(device, obj->handle.framebuffer, NULL);
      break;
   case VK_OBJECT_TYPE_COMMAND_POOL: {
      /* Destroying VkCommandPool frees all VkCommandBuffer objects that were allocated
       * from it.
       */
      vkDestroyCommandPool(device, obj->handle.command_pool, NULL);

      struct vkr_command_buffer *buf, *tmp;
      LIST_FOR_EACH_ENTRY_SAFE (buf, tmp,
                                &((struct vkr_command_pool *)obj)->command_buffers, head)
         util_hash_table_remove_u64(ctx->object_table, buf->base.id);

      break;
   }
   case VK_OBJECT_TYPE_SAMPLER_YCBCR_CONVERSION:
      vkDestroySamplerYcbcrConversion(device, obj->handle.sampler_ycbcr_conversion, NULL);
      break;
   case VK_OBJECT_TYPE_DESCRIPTOR_UPDATE_TEMPLATE:
      vkDestroyDescriptorUpdateTemplate(device, obj->handle.descriptor_update_template,
                                        NULL);
      break;
   case VK_OBJECT_TYPE_INSTANCE:        /* non-device objects */
   case VK_OBJECT_TYPE_PHYSICAL_DEVICE: /* non-device objects */
   case VK_OBJECT_TYPE_DEVICE:          /* device itself */
   case VK_OBJECT_TYPE_QUEUE:           /* not tracked as device objects */
   case VK_OBJECT_TYPE_COMMAND_BUFFER:  /* pool objects */
   case VK_OBJECT_TYPE_DESCRIPTOR_SET:  /* pool objects */
   default:
      vrend_printf("Unhandled vkr_object(%p) with VkObjectType(%u)\n", obj,
                   (uint32_t)obj->type);
      assert(false);
      break;
   };

   list_del(&obj->track_head);

   util_hash_table_remove_u64(ctx->object_table, obj->id);
}

static void
vkr_device_destroy(struct vkr_context *ctx, struct vkr_device *dev)
{
   VkDevice device = dev->base.handle.device;
   VkResult ret = vkDeviceWaitIdle(device);
   if (ret != VK_SUCCESS)
      vrend_printf("vkDeviceWaitIdle(%p) failed(%d)", dev, (int32_t)ret);

   if (!LIST_IS_EMPTY(&dev->objects)) {
      vrend_printf("destroying device with valid objects");

      struct vkr_object *obj, *obj_tmp;
      LIST_FOR_EACH_ENTRY_SAFE (obj, obj_tmp, &dev->objects, track_head)
         vkr_device_object_destroy(ctx, dev, obj);
   }

   struct vkr_queue *queue, *queue_tmp;
   LIST_FOR_EACH_ENTRY_SAFE (queue, queue_tmp, &dev->queues, head)
      vkr_queue_destroy(ctx, queue);

   struct vkr_queue_sync *sync, *sync_tmp;
   LIST_FOR_EACH_ENTRY_SAFE (sync, sync_tmp, &dev->free_syncs, head) {
      vkDestroyFence(dev->base.handle.device, sync->fence, NULL);
      free(sync);
   }

   mtx_destroy(&dev->free_sync_mutex);

   vkDestroyDevice(device, NULL);

   list_del(&dev->base.track_head);

   util_hash_table_remove_u64(ctx->object_table, dev->base.id);
}

static void
vkr_dispatch_vkDestroyDevice(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkDestroyDevice *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      if (dev)
         vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

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

static void
vkr_dispatch_vkGetDeviceQueue(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkGetDeviceQueue *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   const vkr_object_id id =
      vkr_cs_handle_load_id((const void **)args->pQueue, VK_OBJECT_TYPE_QUEUE);

   VkQueue handle;
   vn_replace_vkGetDeviceQueue_args_handle(args);
   vkGetDeviceQueue(args->device, args->queueFamilyIndex, args->queueIndex, &handle);

   struct vkr_queue *queue =
      vkr_queue_create(ctx, dev, id, handle, args->queueFamilyIndex, args->queueIndex);
   /* TODO create queues with device and deal with failures there */
   if (!queue)
      vrend_printf("failed to create queue\n");
}

static void
vkr_dispatch_vkGetDeviceQueue2(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkGetDeviceQueue2 *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   const vkr_object_id id =
      vkr_cs_handle_load_id((const void **)args->pQueue, VK_OBJECT_TYPE_QUEUE);

   VkQueue handle;
   vn_replace_vkGetDeviceQueue2_args_handle(args);
   vkGetDeviceQueue2(args->device, args->pQueueInfo, &handle);

   /* TODO deal with errors */
   vkr_queue_create(ctx, dev, id, handle, args->pQueueInfo->queueFamilyIndex,
                    args->pQueueInfo->queueIndex);
}

static void
vkr_dispatch_vkQueueSubmit(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkQueueSubmit *args)
{
   vn_replace_vkQueueSubmit_args_handle(args);
   args->ret = vkQueueSubmit(args->queue, args->submitCount, args->pSubmits, args->fence);
}

static void
vkr_dispatch_vkQueueBindSparse(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkQueueBindSparse *args)
{
   vn_replace_vkQueueBindSparse_args_handle(args);
   args->ret =
      vkQueueBindSparse(args->queue, args->bindInfoCount, args->pBindInfo, args->fence);
}

static void
vkr_dispatch_vkQueueWaitIdle(struct vn_dispatch_context *dispatch,
                             UNUSED struct vn_command_vkQueueWaitIdle *args)
{
   struct vkr_context *ctx = dispatch->data;
   /* no blocking call */
   vkr_cs_decoder_set_fatal(&ctx->decoder);
}

static bool
vkr_get_fd_handle_type_from_virgl_fd_type(
   struct vkr_physical_device *dev,
   enum virgl_resource_fd_type fd_type,
   VkExternalMemoryHandleTypeFlagBits *out_handle_type)
{
   assert(dev);
   assert(out_handle_type);

   switch (fd_type) {
   case VIRGL_RESOURCE_FD_DMABUF:
      if (!dev->EXT_external_memory_dma_buf)
         return false;
      *out_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      break;
   case VIRGL_RESOURCE_FD_OPAQUE:
      if (!dev->KHR_external_memory_fd)
         return false;
      *out_handle_type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
      break;
   default:
      return false;
   }

   return true;
}

static void
vkr_dispatch_vkAllocateMemory(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkAllocateMemory *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

#ifdef FORCE_ENABLE_DMABUF
   VkExportMemoryAllocateInfo local_export_info;
   if (dev->physical_device->EXT_external_memory_dma_buf) {
      VkExportMemoryAllocateInfo *export_info = vkr_find_pnext(
         args->pAllocateInfo->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO);
      if (export_info) {
         export_info->handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      } else {
         local_export_info = (const VkExportMemoryAllocateInfo){
            .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
            .pNext = args->pAllocateInfo->pNext,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
         };
         ((VkMemoryAllocateInfo *)args->pAllocateInfo)->pNext = &local_export_info;
      }
   }
#endif

   /* translate VkImportMemoryResourceInfoMESA into VkImportMemoryFdInfoKHR */
   VkImportMemoryResourceInfoMESA *import_resource_info = NULL;
   VkImportMemoryFdInfoKHR import_fd_info = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .fd = -1,
   };
   VkBaseInStructure *pprev = (VkBaseInStructure *)args->pAllocateInfo;
   while (pprev->pNext) {
      if (pprev->pNext->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_RESOURCE_INFO_MESA) {
         import_resource_info = (VkImportMemoryResourceInfoMESA *)pprev->pNext;
         import_fd_info.pNext = pprev->pNext->pNext;
         pprev->pNext = (const struct VkBaseInStructure *)&import_fd_info;
         break;
      }
      pprev = (VkBaseInStructure *)pprev->pNext;
   }
   if (import_resource_info) {
      uint32_t res_id = import_resource_info->resourceId;
      struct vkr_resource_attachment *att =
         util_hash_table_get(ctx->resource_table, uintptr_to_pointer(res_id));
      if (!att) {
         vkr_cs_decoder_set_fatal(&ctx->decoder);
         return;
      }

      enum virgl_resource_fd_type fd_type =
         virgl_resource_export_fd(att->resource, &import_fd_info.fd);
      if (!vkr_get_fd_handle_type_from_virgl_fd_type(dev->physical_device, fd_type,
                                                     &import_fd_info.handleType)) {
         close(import_fd_info.fd);
         args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         return;
      }
   }

   struct vkr_device_memory *mem = calloc(1, sizeof(*mem));
   if (!mem) {
      if (import_resource_info)
         close(import_fd_info.fd);
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
      return;
   }

   mem->base.type = VK_OBJECT_TYPE_DEVICE_MEMORY;
   mem->base.id = vkr_cs_handle_load_id((const void **)args->pMemory, mem->base.type);

   vn_replace_vkAllocateMemory_args_handle(args);
   args->ret = vkAllocateMemory(args->device, args->pAllocateInfo, NULL,
                                &mem->base.handle.device_memory);
   if (args->ret != VK_SUCCESS) {
      if (import_resource_info)
         close(import_fd_info.fd);
      free(mem);
      return;
   }

   const VkPhysicalDeviceMemoryProperties *mem_props =
      &dev->physical_device->memory_properties;
   const uint32_t mt_index = args->pAllocateInfo->memoryTypeIndex;
   const uint32_t property_flags = mem_props->memoryTypes[mt_index].propertyFlags;

   /* get valid fd types */
   uint32_t valid_fd_types = 0;
   const VkBaseInStructure *pnext = args->pAllocateInfo->pNext;
   while (pnext) {
      if (pnext->sType == VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) {
         const VkExportMemoryAllocateInfo *export = (const void *)pnext;

         if (export->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT)
            valid_fd_types |= 1 << VIRGL_RESOURCE_FD_OPAQUE;
         if (export->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT)
            valid_fd_types |= 1 << VIRGL_RESOURCE_FD_DMABUF;

         break;
      }
      pnext = pnext->pNext;
   }

   mem->device = args->device;
   mem->property_flags = property_flags;
   mem->valid_fd_types = valid_fd_types;
   list_inithead(&mem->head);

   list_add(&mem->base.track_head, &dev->objects);

   util_hash_table_set_u64(ctx->object_table, mem->base.id, mem);
}

static void
vkr_dispatch_vkFreeMemory(struct vn_dispatch_context *dispatch,
                          struct vn_command_vkFreeMemory *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device_memory *mem = (struct vkr_device_memory *)(uintptr_t)args->memory;
   if (!mem || mem->base.type != VK_OBJECT_TYPE_DEVICE_MEMORY) {
      if (mem)
         vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkFreeMemory_args_handle(args);
   vkFreeMemory(args->device, args->memory, NULL);

   list_del(&mem->head);
   list_del(&mem->base.track_head);

   util_hash_table_remove_u64(ctx->object_table, mem->base.id);
}

static void
vkr_dispatch_vkGetDeviceMemoryCommitment(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryCommitment *args)
{
   vn_replace_vkGetDeviceMemoryCommitment_args_handle(args);
   vkGetDeviceMemoryCommitment(args->device, args->memory, args->pCommittedMemoryInBytes);
}

static void
vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDeviceMemoryOpaqueCaptureAddress *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkGetDeviceMemoryOpaqueCaptureAddress_args_handle(args);
   args->ret = dev->GetDeviceMemoryOpaqueCaptureAddress(args->device, args->pInfo);
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
vkr_dispatch_vkGetImageMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements *args)
{
   vn_replace_vkGetImageMemoryRequirements_args_handle(args);
   vkGetImageMemoryRequirements(args->device, args->image, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageMemoryRequirements2 *args)
{
   vn_replace_vkGetImageMemoryRequirements2_args_handle(args);
   vkGetImageMemoryRequirements2(args->device, args->pInfo, args->pMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements *args)
{
   vn_replace_vkGetImageSparseMemoryRequirements_args_handle(args);
   vkGetImageSparseMemoryRequirements(args->device, args->image,
                                      args->pSparseMemoryRequirementCount,
                                      args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkGetImageSparseMemoryRequirements2(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSparseMemoryRequirements2 *args)
{
   vn_replace_vkGetImageSparseMemoryRequirements2_args_handle(args);
   vkGetImageSparseMemoryRequirements2(args->device, args->pInfo,
                                       args->pSparseMemoryRequirementCount,
                                       args->pSparseMemoryRequirements);
}

static void
vkr_dispatch_vkBindImageMemory(UNUSED struct vn_dispatch_context *dispatch,
                               struct vn_command_vkBindImageMemory *args)
{
   vn_replace_vkBindImageMemory_args_handle(args);
   args->ret =
      vkBindImageMemory(args->device, args->image, args->memory, args->memoryOffset);
}

static void
vkr_dispatch_vkBindImageMemory2(UNUSED struct vn_dispatch_context *dispatch,
                                struct vn_command_vkBindImageMemory2 *args)
{
   vn_replace_vkBindImageMemory2_args_handle(args);
   args->ret = vkBindImageMemory2(args->device, args->bindInfoCount, args->pBindInfos);
}

static void
vkr_dispatch_vkGetImageSubresourceLayout(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageSubresourceLayout *args)
{
   vn_replace_vkGetImageSubresourceLayout_args_handle(args);
   vkGetImageSubresourceLayout(args->device, args->image, args->pSubresource,
                               args->pLayout);
}

static void
vkr_dispatch_vkCreateFence(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateFence *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(fence, fence, FENCE, vkCreateFence, pFence);

   util_hash_table_set_u64(ctx->object_table, fence->base.id, fence);
}

static void
vkr_dispatch_vkDestroyFence(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyFence *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(fence, fence, FENCE, vkDestroyFence, fence);

   util_hash_table_remove_u64(ctx->object_table, fence->base.id);
}

static void
vkr_dispatch_vkResetFences(UNUSED struct vn_dispatch_context *dispatch,
                           struct vn_command_vkResetFences *args)
{
   vn_replace_vkResetFences_args_handle(args);
   args->ret = vkResetFences(args->device, args->fenceCount, args->pFences);
}

static void
vkr_dispatch_vkGetFenceStatus(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkGetFenceStatus *args)
{
   vn_replace_vkGetFenceStatus_args_handle(args);
   args->ret = vkGetFenceStatus(args->device, args->fence);
}

static void
vkr_dispatch_vkWaitForFences(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkWaitForFences *args)
{
   struct vkr_context *ctx = dispatch->data;

   /* Being single-threaded, we cannot afford potential blocking calls.  It
    * also leads to GPU lost when the wait never returns and can only be
    * unblocked by a following command (e.g., vkCmdWaitEvents that is
    * unblocked by a following vkSetEvent).
    */
   if (args->timeout) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkWaitForFences_args_handle(args);
   args->ret = vkWaitForFences(args->device, args->fenceCount, args->pFences,
                               args->waitAll, args->timeout);
}

static void
vkr_dispatch_vkCreateSemaphore(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateSemaphore *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(sem, semaphore, SEMAPHORE, vkCreateSemaphore, pSemaphore);

   util_hash_table_set_u64(ctx->object_table, sem->base.id, sem);
}

static void
vkr_dispatch_vkDestroySemaphore(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroySemaphore *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(sem, semaphore, SEMAPHORE, vkDestroySemaphore, semaphore);

   util_hash_table_remove_u64(ctx->object_table, sem->base.id);
}

static void
vkr_dispatch_vkGetSemaphoreCounterValue(struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkGetSemaphoreCounterValue *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkGetSemaphoreCounterValue_args_handle(args);
   args->ret = dev->GetSemaphoreCounterValue(args->device, args->semaphore, args->pValue);
}

static void
vkr_dispatch_vkWaitSemaphores(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkWaitSemaphores *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = (struct vkr_device *)args->device;

   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   /* no blocking call */
   if (args->timeout) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkWaitSemaphores_args_handle(args);
   args->ret = dev->WaitSemaphores(args->device, args->pWaitInfo, args->timeout);
}

static void
vkr_dispatch_vkSignalSemaphore(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkSignalSemaphore *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkSignalSemaphore_args_handle(args);
   args->ret = dev->SignalSemaphore(args->device, args->pSignalInfo);
}

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

static void
vkr_dispatch_vkCreateImage(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateImage *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

#ifdef FORCE_ENABLE_DMABUF
   /* Do not chain VkExternalMemoryImageCreateInfo with optimal tiling, so that
    * guest Venus can pass memory requirement cts with dedicated allocation.
    */
   VkExternalMemoryImageCreateInfo local_external_info;
   if (args->pCreateInfo->tiling != VK_IMAGE_TILING_OPTIMAL &&
       dev->physical_device->EXT_external_memory_dma_buf) {
      VkExternalMemoryImageCreateInfo *external_info = vkr_find_pnext(
         args->pCreateInfo->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
      if (external_info) {
         external_info->handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
      } else {
         local_external_info = (const VkExternalMemoryImageCreateInfo){
            .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .pNext = args->pCreateInfo->pNext,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
         };
         ((VkImageCreateInfo *)args->pCreateInfo)->pNext = &local_external_info;
      }
   }
#endif

   CREATE_OBJECT(img, image, IMAGE, vkCreateImage, pImage);

   util_hash_table_set_u64(ctx->object_table, img->base.id, img);
}

static void
vkr_dispatch_vkDestroyImage(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyImage *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(img, image, IMAGE, vkDestroyImage, image);

   util_hash_table_remove_u64(ctx->object_table, img->base.id);
}

static void
vkr_dispatch_vkCreateImageView(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateImageView *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(view, image_view, IMAGE_VIEW, vkCreateImageView, pView);

   util_hash_table_set_u64(ctx->object_table, view->base.id, view);
}

static void
vkr_dispatch_vkDestroyImageView(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroyImageView *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(view, image_view, IMAGE_VIEW, vkDestroyImageView, imageView);

   util_hash_table_remove_u64(ctx->object_table, view->base.id);
}

static void
vkr_dispatch_vkCreateSampler(struct vn_dispatch_context *dispatch,
                             struct vn_command_vkCreateSampler *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(sampler, sampler, SAMPLER, vkCreateSampler, pSampler);

   util_hash_table_set_u64(ctx->object_table, sampler->base.id, sampler);
}

static void
vkr_dispatch_vkDestroySampler(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkDestroySampler *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(sampler, sampler, SAMPLER, vkDestroySampler, sampler);

   util_hash_table_remove_u64(ctx->object_table, sampler->base.id);
}

static void
vkr_dispatch_vkCreateSamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateSamplerYcbcrConversion *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(conv, sampler_ycbcr_conversion, SAMPLER_YCBCR_CONVERSION,
                 vkCreateSamplerYcbcrConversion, pYcbcrConversion);

   util_hash_table_set_u64(ctx->object_table, conv->base.id, conv);
}

static void
vkr_dispatch_vkDestroySamplerYcbcrConversion(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroySamplerYcbcrConversion *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(conv, sampler_ycbcr_conversion, SAMPLER_YCBCR_CONVERSION,
                  vkDestroySamplerYcbcrConversion, ycbcrConversion);

   util_hash_table_remove_u64(ctx->object_table, conv->base.id);
}

static void
vkr_dispatch_vkGetDescriptorSetLayoutSupport(
   UNUSED struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetDescriptorSetLayoutSupport *args)
{
   vn_replace_vkGetDescriptorSetLayoutSupport_args_handle(args);
   vkGetDescriptorSetLayoutSupport(args->device, args->pCreateInfo, args->pSupport);
}

static void
vkr_dispatch_vkCreateDescriptorSetLayout(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateDescriptorSetLayout *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(layout, descriptor_set_layout, DESCRIPTOR_SET_LAYOUT,
                 vkCreateDescriptorSetLayout, pSetLayout);

   util_hash_table_set_u64(ctx->object_table, layout->base.id, layout);
}

static void
vkr_dispatch_vkDestroyDescriptorSetLayout(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroyDescriptorSetLayout *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(layout, descriptor_set_layout, DESCRIPTOR_SET_LAYOUT,
                  vkDestroyDescriptorSetLayout, descriptorSetLayout);

   util_hash_table_remove_u64(ctx->object_table, layout->base.id);
}

static void
vkr_dispatch_vkCreateDescriptorPool(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCreateDescriptorPool *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(pool, descriptor_pool, DESCRIPTOR_POOL, vkCreateDescriptorPool,
                 pDescriptorPool);

   list_inithead(&pool->descriptor_sets);

   util_hash_table_set_u64(ctx->object_table, pool->base.id, pool);
}

static void
vkr_dispatch_vkDestroyDescriptorPool(struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkDestroyDescriptorPool *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(pool, descriptor_pool, DESCRIPTOR_POOL, vkDestroyDescriptorPool,
                  descriptorPool);

   struct vkr_descriptor_set *set, *tmp;
   LIST_FOR_EACH_ENTRY_SAFE (set, tmp, &pool->descriptor_sets, head)
      util_hash_table_remove_u64(ctx->object_table, set->base.id);

   util_hash_table_remove_u64(ctx->object_table, pool->base.id);
}

static void
vkr_dispatch_vkResetDescriptorPool(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkResetDescriptorPool *args)
{
   struct vkr_context *ctx = dispatch->data;

   struct vkr_descriptor_pool *pool =
      (struct vkr_descriptor_pool *)(uintptr_t)args->descriptorPool;
   if (!pool || pool->base.type != VK_OBJECT_TYPE_DESCRIPTOR_POOL) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkResetDescriptorPool_args_handle(args);
   args->ret = vkResetDescriptorPool(args->device, args->descriptorPool, args->flags);

   struct vkr_descriptor_set *set, *tmp;
   LIST_FOR_EACH_ENTRY_SAFE (set, tmp, &pool->descriptor_sets, head)
      util_hash_table_remove_u64(ctx->object_table, set->base.id);
   list_inithead(&pool->descriptor_sets);
}

static void
vkr_dispatch_vkAllocateDescriptorSets(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkAllocateDescriptorSets *args)
{
   struct vkr_context *ctx = dispatch->data;

   ALLOCATE_OBJECT_ARRAY(set, descriptor_set, DESCRIPTOR_SET, DescriptorSet,
                         vkAllocateDescriptorSets, descriptorSetCount, descriptorPool,
                         descriptor_pool, DESCRIPTOR_POOL);
}

static void
vkr_dispatch_vkFreeDescriptorSets(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkFreeDescriptorSets *args)
{
   struct vkr_context *ctx = dispatch->data;

   FREE_OBJECT_ARRAY(set, descriptor_set, DESCRIPTOR_SET, vkFreeDescriptorSets,
                     pDescriptorSets, descriptorSetCount, descriptorPool);

   args->ret = VK_SUCCESS;
}

static void
vkr_dispatch_vkUpdateDescriptorSets(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkUpdateDescriptorSets *args)
{
   vn_replace_vkUpdateDescriptorSets_args_handle(args);
   vkUpdateDescriptorSets(args->device, args->descriptorWriteCount,
                          args->pDescriptorWrites, args->descriptorCopyCount,
                          args->pDescriptorCopies);
}

static void
vkr_dispatch_vkCreateDescriptorUpdateTemplate(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkCreateDescriptorUpdateTemplate *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(templ, descriptor_update_template, DESCRIPTOR_UPDATE_TEMPLATE,
                 vkCreateDescriptorUpdateTemplate, pDescriptorUpdateTemplate);

   util_hash_table_set_u64(ctx->object_table, templ->base.id, templ);
}

static void
vkr_dispatch_vkDestroyDescriptorUpdateTemplate(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkDestroyDescriptorUpdateTemplate *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(templ, descriptor_update_template, DESCRIPTOR_UPDATE_TEMPLATE,
                  vkDestroyDescriptorUpdateTemplate, descriptorUpdateTemplate);

   util_hash_table_remove_u64(ctx->object_table, templ->base.id);
}

static void
vkr_dispatch_vkCreateRenderPass(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCreateRenderPass *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(pass, render_pass, RENDER_PASS, vkCreateRenderPass, pRenderPass);

   util_hash_table_set_u64(ctx->object_table, pass->base.id, pass);
}

static void
vkr_dispatch_vkCreateRenderPass2(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCreateRenderPass2 *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   struct vkr_render_pass *pass = calloc(1, sizeof(*pass));
   if (!pass) {
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;
      return;
   }
   pass->base.type = VK_OBJECT_TYPE_RENDER_PASS;
   pass->base.id =
      vkr_cs_handle_load_id((const void **)args->pRenderPass, pass->base.type);

   vn_replace_vkCreateRenderPass2_args_handle(args);
   args->ret = dev->CreateRenderPass2(args->device, args->pCreateInfo, NULL,
                                      &pass->base.handle.render_pass);
   if (args->ret != VK_SUCCESS) {
      free(pass);
      return;
   }

   list_add(&pass->base.track_head, &dev->objects);

   util_hash_table_set_u64(ctx->object_table, pass->base.id, pass);
}

static void
vkr_dispatch_vkDestroyRenderPass(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkDestroyRenderPass *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(pass, render_pass, RENDER_PASS, vkDestroyRenderPass, renderPass);

   util_hash_table_remove_u64(ctx->object_table, pass->base.id);
}

static void
vkr_dispatch_vkGetRenderAreaGranularity(UNUSED struct vn_dispatch_context *dispatch,
                                        struct vn_command_vkGetRenderAreaGranularity *args)
{
   vn_replace_vkGetRenderAreaGranularity_args_handle(args);
   vkGetRenderAreaGranularity(args->device, args->renderPass, args->pGranularity);
}

static void
vkr_dispatch_vkCreateFramebuffer(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkCreateFramebuffer *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(fb, framebuffer, FRAMEBUFFER, vkCreateFramebuffer, pFramebuffer);

   util_hash_table_set_u64(ctx->object_table, fb->base.id, fb);
}

static void
vkr_dispatch_vkDestroyFramebuffer(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkDestroyFramebuffer *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(fb, framebuffer, FRAMEBUFFER, vkDestroyFramebuffer, framebuffer);

   util_hash_table_remove_u64(ctx->object_table, fb->base.id);
}

static void
vkr_dispatch_vkCreateEvent(struct vn_dispatch_context *dispatch,
                           struct vn_command_vkCreateEvent *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(ev, event, EVENT, vkCreateEvent, pEvent);

   util_hash_table_set_u64(ctx->object_table, ev->base.id, ev);
}

static void
vkr_dispatch_vkDestroyEvent(struct vn_dispatch_context *dispatch,
                            struct vn_command_vkDestroyEvent *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(ev, event, EVENT, vkDestroyEvent, event);

   util_hash_table_remove_u64(ctx->object_table, ev->base.id);
}

static void
vkr_dispatch_vkGetEventStatus(UNUSED struct vn_dispatch_context *dispatch,
                              struct vn_command_vkGetEventStatus *args)
{
   vn_replace_vkGetEventStatus_args_handle(args);
   args->ret = vkGetEventStatus(args->device, args->event);
}

static void
vkr_dispatch_vkSetEvent(UNUSED struct vn_dispatch_context *dispatch,
                        struct vn_command_vkSetEvent *args)
{
   vn_replace_vkSetEvent_args_handle(args);
   args->ret = vkSetEvent(args->device, args->event);
}

static void
vkr_dispatch_vkResetEvent(UNUSED struct vn_dispatch_context *dispatch,
                          struct vn_command_vkResetEvent *args)
{
   vn_replace_vkResetEvent_args_handle(args);
   args->ret = vkResetEvent(args->device, args->event);
}

static void
vkr_dispatch_vkCreateQueryPool(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateQueryPool *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(pool, query_pool, QUERY_POOL, vkCreateQueryPool, pQueryPool);

   util_hash_table_set_u64(ctx->object_table, pool->base.id, pool);
}

static void
vkr_dispatch_vkDestroyQueryPool(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroyQueryPool *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(pool, query_pool, QUERY_POOL, vkDestroyQueryPool, queryPool);

   util_hash_table_remove_u64(ctx->object_table, pool->base.id);
}

static void
vkr_dispatch_vkGetQueryPoolResults(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkGetQueryPoolResults *args)
{
   vn_replace_vkGetQueryPoolResults_args_handle(args);
   args->ret = vkGetQueryPoolResults(args->device, args->queryPool, args->firstQuery,
                                     args->queryCount, args->dataSize, args->pData,
                                     args->stride, args->flags);
}

static void
vkr_dispatch_vkResetQueryPool(struct vn_dispatch_context *dispatch,
                              struct vn_command_vkResetQueryPool *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkResetQueryPool_args_handle(args);
   dev->ResetQueryPool(args->device, args->queryPool, args->firstQuery, args->queryCount);
}

static void
vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetImageDrmFormatModifierPropertiesEXT *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   vn_replace_vkGetImageDrmFormatModifierPropertiesEXT_args_handle(args);
   args->ret = dev->get_image_drm_format_modifier_properties(args->device, args->image,
                                                             args->pProperties);
}

static void
vkr_dispatch_vkGetMemoryResourcePropertiesMESA(
   struct vn_dispatch_context *dispatch,
   struct vn_command_vkGetMemoryResourcePropertiesMESA *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = (struct vkr_device *)args->device;
   if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   struct vkr_resource_attachment *att =
      util_hash_table_get(ctx->resource_table, uintptr_to_pointer(args->resourceId));
   if (!att) {
      vkr_cs_decoder_set_fatal(&ctx->decoder);
      return;
   }

   int fd = -1;
   enum virgl_resource_fd_type fd_type = virgl_resource_export_fd(att->resource, &fd);
   VkExternalMemoryHandleTypeFlagBits handle_type;
   if (!vkr_get_fd_handle_type_from_virgl_fd_type(dev->physical_device, fd_type,
                                                  &handle_type) ||
       handle_type != VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) {
      close(fd);
      args->ret = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      return;
   }

   VkMemoryFdPropertiesKHR mem_fd_props = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
      .pNext = NULL,
      .memoryTypeBits = 0,
   };
   vn_replace_vkGetMemoryResourcePropertiesMESA_args_handle(args);
   args->ret =
      dev->get_memory_fd_properties(args->device, handle_type, fd, &mem_fd_props);
   if (args->ret != VK_SUCCESS) {
      close(fd);
      return;
   }

   args->pMemoryResourceProperties->memoryTypeBits = mem_fd_props.memoryTypeBits;

   VkMemoryResourceAllocationSizeProperties100000MESA *alloc_size_props = vkr_find_pnext(
      args->pMemoryResourceProperties->pNext,
      VK_STRUCTURE_TYPE_MEMORY_RESOURCE_ALLOCATION_SIZE_PROPERTIES_100000_MESA);
   if (alloc_size_props)
      alloc_size_props->allocationSize = lseek(fd, 0, SEEK_END);

   close(fd);
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

   dispatch->dispatch_vkEnumerateInstanceVersion =
      vkr_dispatch_vkEnumerateInstanceVersion;
   dispatch->dispatch_vkEnumerateInstanceExtensionProperties =
      vkr_dispatch_vkEnumerateInstanceExtensionProperties;
   /* we don't advertise layers (and should never) */
   dispatch->dispatch_vkEnumerateInstanceLayerProperties = NULL;
   dispatch->dispatch_vkCreateInstance = vkr_dispatch_vkCreateInstance;
   dispatch->dispatch_vkDestroyInstance = vkr_dispatch_vkDestroyInstance;
   dispatch->dispatch_vkGetInstanceProcAddr = NULL;

   dispatch->dispatch_vkEnumeratePhysicalDevices =
      vkr_dispatch_vkEnumeratePhysicalDevices;
   dispatch->dispatch_vkEnumeratePhysicalDeviceGroups =
      vkr_dispatch_vkEnumeratePhysicalDeviceGroups;
   dispatch->dispatch_vkGetPhysicalDeviceFeatures =
      vkr_dispatch_vkGetPhysicalDeviceFeatures;
   dispatch->dispatch_vkGetPhysicalDeviceProperties =
      vkr_dispatch_vkGetPhysicalDeviceProperties;
   dispatch->dispatch_vkGetPhysicalDeviceQueueFamilyProperties =
      vkr_dispatch_vkGetPhysicalDeviceQueueFamilyProperties;
   dispatch->dispatch_vkGetPhysicalDeviceMemoryProperties =
      vkr_dispatch_vkGetPhysicalDeviceMemoryProperties;
   dispatch->dispatch_vkGetPhysicalDeviceFormatProperties =
      vkr_dispatch_vkGetPhysicalDeviceFormatProperties;
   dispatch->dispatch_vkGetPhysicalDeviceImageFormatProperties =
      vkr_dispatch_vkGetPhysicalDeviceImageFormatProperties;
   dispatch->dispatch_vkGetPhysicalDeviceSparseImageFormatProperties =
      vkr_dispatch_vkGetPhysicalDeviceSparseImageFormatProperties;
   dispatch->dispatch_vkGetPhysicalDeviceFeatures2 =
      vkr_dispatch_vkGetPhysicalDeviceFeatures2;
   dispatch->dispatch_vkGetPhysicalDeviceProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceQueueFamilyProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceQueueFamilyProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceMemoryProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceMemoryProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceFormatProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceFormatProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceImageFormatProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceImageFormatProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceSparseImageFormatProperties2 =
      vkr_dispatch_vkGetPhysicalDeviceSparseImageFormatProperties2;
   dispatch->dispatch_vkGetPhysicalDeviceExternalBufferProperties =
      vkr_dispatch_vkGetPhysicalDeviceExternalBufferProperties;
   dispatch->dispatch_vkGetMemoryFdKHR = NULL;
   dispatch->dispatch_vkGetMemoryFdPropertiesKHR = NULL;
   dispatch->dispatch_vkGetPhysicalDeviceExternalSemaphoreProperties =
      vkr_dispatch_vkGetPhysicalDeviceExternalSemaphoreProperties;
   dispatch->dispatch_vkGetPhysicalDeviceExternalFenceProperties =
      vkr_dispatch_vkGetPhysicalDeviceExternalFenceProperties;

   dispatch->dispatch_vkEnumerateDeviceExtensionProperties =
      vkr_dispatch_vkEnumerateDeviceExtensionProperties;
   dispatch->dispatch_vkEnumerateDeviceLayerProperties = NULL;
   dispatch->dispatch_vkCreateDevice = vkr_dispatch_vkCreateDevice;
   dispatch->dispatch_vkDestroyDevice = vkr_dispatch_vkDestroyDevice;
   dispatch->dispatch_vkGetDeviceProcAddr = NULL;
   dispatch->dispatch_vkGetDeviceGroupPeerMemoryFeatures =
      vkr_dispatch_vkGetDeviceGroupPeerMemoryFeatures;
   dispatch->dispatch_vkDeviceWaitIdle = vkr_dispatch_vkDeviceWaitIdle;

   dispatch->dispatch_vkGetDeviceQueue = vkr_dispatch_vkGetDeviceQueue;
   dispatch->dispatch_vkGetDeviceQueue2 = vkr_dispatch_vkGetDeviceQueue2;
   dispatch->dispatch_vkQueueSubmit = vkr_dispatch_vkQueueSubmit;
   dispatch->dispatch_vkQueueBindSparse = vkr_dispatch_vkQueueBindSparse;
   dispatch->dispatch_vkQueueWaitIdle = vkr_dispatch_vkQueueWaitIdle;

   dispatch->dispatch_vkCreateFence = vkr_dispatch_vkCreateFence;
   dispatch->dispatch_vkDestroyFence = vkr_dispatch_vkDestroyFence;
   dispatch->dispatch_vkResetFences = vkr_dispatch_vkResetFences;
   dispatch->dispatch_vkGetFenceStatus = vkr_dispatch_vkGetFenceStatus;
   dispatch->dispatch_vkWaitForFences = vkr_dispatch_vkWaitForFences;

   dispatch->dispatch_vkCreateSemaphore = vkr_dispatch_vkCreateSemaphore;
   dispatch->dispatch_vkDestroySemaphore = vkr_dispatch_vkDestroySemaphore;
   dispatch->dispatch_vkGetSemaphoreCounterValue =
      vkr_dispatch_vkGetSemaphoreCounterValue;
   dispatch->dispatch_vkWaitSemaphores = vkr_dispatch_vkWaitSemaphores;
   dispatch->dispatch_vkSignalSemaphore = vkr_dispatch_vkSignalSemaphore;

   dispatch->dispatch_vkAllocateMemory = vkr_dispatch_vkAllocateMemory;
   dispatch->dispatch_vkFreeMemory = vkr_dispatch_vkFreeMemory;
   dispatch->dispatch_vkMapMemory = NULL;
   dispatch->dispatch_vkUnmapMemory = NULL;
   dispatch->dispatch_vkFlushMappedMemoryRanges = NULL;
   dispatch->dispatch_vkInvalidateMappedMemoryRanges = NULL;
   dispatch->dispatch_vkGetDeviceMemoryCommitment =
      vkr_dispatch_vkGetDeviceMemoryCommitment;
   dispatch->dispatch_vkGetDeviceMemoryOpaqueCaptureAddress =
      vkr_dispatch_vkGetDeviceMemoryOpaqueCaptureAddress;

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

   dispatch->dispatch_vkCreateBufferView = vkr_dispatch_vkCreateBufferView;
   dispatch->dispatch_vkDestroyBufferView = vkr_dispatch_vkDestroyBufferView;

   dispatch->dispatch_vkCreateImage = vkr_dispatch_vkCreateImage;
   dispatch->dispatch_vkDestroyImage = vkr_dispatch_vkDestroyImage;
   dispatch->dispatch_vkGetImageMemoryRequirements =
      vkr_dispatch_vkGetImageMemoryRequirements;
   dispatch->dispatch_vkGetImageMemoryRequirements2 =
      vkr_dispatch_vkGetImageMemoryRequirements2;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements =
      vkr_dispatch_vkGetImageSparseMemoryRequirements;
   dispatch->dispatch_vkGetImageSparseMemoryRequirements2 =
      vkr_dispatch_vkGetImageSparseMemoryRequirements2;
   dispatch->dispatch_vkBindImageMemory = vkr_dispatch_vkBindImageMemory;
   dispatch->dispatch_vkBindImageMemory2 = vkr_dispatch_vkBindImageMemory2;
   dispatch->dispatch_vkGetImageSubresourceLayout =
      vkr_dispatch_vkGetImageSubresourceLayout;

   dispatch->dispatch_vkCreateImageView = vkr_dispatch_vkCreateImageView;
   dispatch->dispatch_vkDestroyImageView = vkr_dispatch_vkDestroyImageView;

   dispatch->dispatch_vkCreateSampler = vkr_dispatch_vkCreateSampler;
   dispatch->dispatch_vkDestroySampler = vkr_dispatch_vkDestroySampler;

   dispatch->dispatch_vkCreateSamplerYcbcrConversion =
      vkr_dispatch_vkCreateSamplerYcbcrConversion;
   dispatch->dispatch_vkDestroySamplerYcbcrConversion =
      vkr_dispatch_vkDestroySamplerYcbcrConversion;

   dispatch->dispatch_vkGetDescriptorSetLayoutSupport =
      vkr_dispatch_vkGetDescriptorSetLayoutSupport;
   dispatch->dispatch_vkCreateDescriptorSetLayout =
      vkr_dispatch_vkCreateDescriptorSetLayout;
   dispatch->dispatch_vkDestroyDescriptorSetLayout =
      vkr_dispatch_vkDestroyDescriptorSetLayout;

   dispatch->dispatch_vkCreateDescriptorPool = vkr_dispatch_vkCreateDescriptorPool;
   dispatch->dispatch_vkDestroyDescriptorPool = vkr_dispatch_vkDestroyDescriptorPool;
   dispatch->dispatch_vkResetDescriptorPool = vkr_dispatch_vkResetDescriptorPool;

   dispatch->dispatch_vkAllocateDescriptorSets = vkr_dispatch_vkAllocateDescriptorSets;
   dispatch->dispatch_vkFreeDescriptorSets = vkr_dispatch_vkFreeDescriptorSets;
   dispatch->dispatch_vkUpdateDescriptorSets = vkr_dispatch_vkUpdateDescriptorSets;

   dispatch->dispatch_vkCreateDescriptorUpdateTemplate =
      vkr_dispatch_vkCreateDescriptorUpdateTemplate;
   dispatch->dispatch_vkDestroyDescriptorUpdateTemplate =
      vkr_dispatch_vkDestroyDescriptorUpdateTemplate;
   dispatch->dispatch_vkUpdateDescriptorSetWithTemplate = NULL;

   dispatch->dispatch_vkCreateRenderPass = vkr_dispatch_vkCreateRenderPass;
   dispatch->dispatch_vkCreateRenderPass2 = vkr_dispatch_vkCreateRenderPass2;
   dispatch->dispatch_vkDestroyRenderPass = vkr_dispatch_vkDestroyRenderPass;
   dispatch->dispatch_vkGetRenderAreaGranularity =
      vkr_dispatch_vkGetRenderAreaGranularity;

   dispatch->dispatch_vkCreateFramebuffer = vkr_dispatch_vkCreateFramebuffer;
   dispatch->dispatch_vkDestroyFramebuffer = vkr_dispatch_vkDestroyFramebuffer;

   dispatch->dispatch_vkCreateEvent = vkr_dispatch_vkCreateEvent;
   dispatch->dispatch_vkDestroyEvent = vkr_dispatch_vkDestroyEvent;
   dispatch->dispatch_vkGetEventStatus = vkr_dispatch_vkGetEventStatus;
   dispatch->dispatch_vkSetEvent = vkr_dispatch_vkSetEvent;
   dispatch->dispatch_vkResetEvent = vkr_dispatch_vkResetEvent;

   dispatch->dispatch_vkCreateQueryPool = vkr_dispatch_vkCreateQueryPool;
   dispatch->dispatch_vkDestroyQueryPool = vkr_dispatch_vkDestroyQueryPool;
   dispatch->dispatch_vkGetQueryPoolResults = vkr_dispatch_vkGetQueryPoolResults;
   dispatch->dispatch_vkResetQueryPool = vkr_dispatch_vkResetQueryPool;

   vkr_context_init_shader_module_dispatch(ctx);
   vkr_context_init_pipeline_layout_dispatch(ctx);
   vkr_context_init_pipeline_cache_dispatch(ctx);
   vkr_context_init_pipeline_dispatch(ctx);

   vkr_context_init_command_pool_dispatch(ctx);
   vkr_context_init_command_buffer_dispatch(ctx);

   dispatch->dispatch_vkGetImageDrmFormatModifierPropertiesEXT =
      vkr_dispatch_vkGetImageDrmFormatModifierPropertiesEXT;

   dispatch->dispatch_vkGetMemoryResourcePropertiesMESA =
      vkr_dispatch_vkGetMemoryResourcePropertiesMESA;

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
