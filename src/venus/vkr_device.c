/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_device.h"

#include "venus-protocol/vn_protocol_renderer_device.h"
#include "venus-protocol/vn_protocol_renderer_info.h"
#include "venus-protocol/vn_protocol_renderer_instance.h"

#include "vkr_command_buffer.h"
#include "vkr_context.h"
#include "vkr_descriptor_set.h"
#include "vkr_device_memory.h"
#include "vkr_queue.h"

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

void
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

void
vkr_context_init_instance_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkEnumerateInstanceVersion =
      vkr_dispatch_vkEnumerateInstanceVersion;
   dispatch->dispatch_vkEnumerateInstanceExtensionProperties =
      vkr_dispatch_vkEnumerateInstanceExtensionProperties;
   /* we don't advertise layers (and should never) */
   dispatch->dispatch_vkEnumerateInstanceLayerProperties = NULL;
   dispatch->dispatch_vkCreateInstance = vkr_dispatch_vkCreateInstance;
   dispatch->dispatch_vkDestroyInstance = vkr_dispatch_vkDestroyInstance;
   dispatch->dispatch_vkGetInstanceProcAddr = NULL;
}

void
vkr_context_init_physical_device_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

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
}

void
vkr_context_init_device_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkEnumerateDeviceExtensionProperties =
      vkr_dispatch_vkEnumerateDeviceExtensionProperties;
   dispatch->dispatch_vkEnumerateDeviceLayerProperties = NULL;
   dispatch->dispatch_vkCreateDevice = vkr_dispatch_vkCreateDevice;
   dispatch->dispatch_vkDestroyDevice = vkr_dispatch_vkDestroyDevice;
   dispatch->dispatch_vkGetDeviceProcAddr = NULL;
   dispatch->dispatch_vkGetDeviceGroupPeerMemoryFeatures =
      vkr_dispatch_vkGetDeviceGroupPeerMemoryFeatures;
   dispatch->dispatch_vkDeviceWaitIdle = vkr_dispatch_vkDeviceWaitIdle;
}
