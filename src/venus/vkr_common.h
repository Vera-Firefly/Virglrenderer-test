/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_COMMON_H
#define VKR_COMMON_H

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "c11/threads.h"
#include "os/os_misc.h"
#include "os/os_thread.h"
#include "pipe/p_compiler.h"
#include "util/u_double_list.h"
#include "util/u_hash_table.h"
#include "util/u_math.h"
#include "util/u_memory.h"
#include "util/u_pointer.h"
#include "venus-protocol/vulkan.h"
#include "virgl_util.h"
#include "virglrenderer.h"
#include "vrend_debug.h"

#include "vkr_renderer.h"

/*
 * TODO what extensions do we need from the host driver?
 *
 * We don't check vkGetPhysicalDeviceExternalBufferProperties, etc. yet.  Even
 * if we did, silently adding external memory info to vkCreateBuffer or
 * vkCreateImage could change the results of vkGetBufferMemoryRequirements or
 * vkGetImageMemoryRequirements and confuse the guest.
 */
#define FORCE_ENABLE_DMABUF

#define VKR_DEBUG(category) (unlikely(vkr_debug_flags & VKR_DEBUG_##category))

/*
 * TODO Most of the functions are generated.  Some of them are then
 * hand-edited.  Find a better/cleaner way to reduce manual works.
 */
#define CREATE_OBJECT(obj, vkr_type, vk_obj, vk_cmd, vk_arg)                             \
   struct vkr_device *dev_obj = (struct vkr_device *)args->device;                       \
   if (!dev_obj || dev_obj->base.type != VK_OBJECT_TYPE_DEVICE) {                        \
      vkr_cs_decoder_set_fatal(&ctx->decoder);                                           \
      return;                                                                            \
   }                                                                                     \
                                                                                         \
   struct vkr_##vkr_type *obj = calloc(1, sizeof(*obj));                                 \
   if (!obj) {                                                                           \
      args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;                                           \
      return;                                                                            \
   }                                                                                     \
   obj->base.type = VK_OBJECT_TYPE_##vk_obj;                                             \
   obj->base.id = vkr_cs_handle_load_id((const void **)args->vk_arg, obj->base.type);    \
                                                                                         \
   vn_replace_##vk_cmd##_args_handle(args);                                              \
   args->ret =                                                                           \
      vk_cmd(args->device, args->pCreateInfo, NULL, &obj->base.handle.vkr_type);         \
   if (args->ret != VK_SUCCESS) {                                                        \
      free(obj);                                                                         \
      return;                                                                            \
   }                                                                                     \
                                                                                         \
   list_add(&obj->base.track_head, &dev_obj->objects)

#define DESTROY_OBJECT(obj, vkr_type, vk_obj, vk_cmd, vk_arg)                            \
   struct vkr_##vkr_type *obj = (struct vkr_##vkr_type *)(uintptr_t)args->vk_arg;        \
   if (!obj || obj->base.type != VK_OBJECT_TYPE_##vk_obj) {                              \
      if (obj)                                                                           \
         vkr_cs_decoder_set_fatal(&ctx->decoder);                                        \
      return;                                                                            \
   }                                                                                     \
                                                                                         \
   vn_replace_##vk_cmd##_args_handle(args);                                              \
   vk_cmd(args->device, args->vk_arg, NULL);                                             \
                                                                                         \
   list_del(&obj->base.track_head)

#define ALLOCATE_POOL_OBJECTS(obj, vkr_type, vk_type, vk_obj, vk_cmd, arg_count,         \
                              arg_pool, vkr_pool_type, vk_pool_type)                     \
   do {                                                                                  \
      struct vkr_device *dev = (struct vkr_device *)args->device;                        \
      if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {                             \
         vkr_cs_decoder_set_fatal(&ctx->decoder);                                        \
         return;                                                                         \
      }                                                                                  \
                                                                                         \
      struct vkr_##vkr_pool_type *pool =                                                 \
         (struct vkr_##vkr_pool_type *)(uintptr_t)args->pAllocateInfo->arg_pool;         \
      if (!pool || pool->base.type != VK_OBJECT_TYPE_##vk_pool_type) {                   \
         vkr_cs_decoder_set_fatal(&ctx->decoder);                                        \
         return;                                                                         \
      }                                                                                  \
                                                                                         \
      struct object_array arr;                                                           \
      if (!object_array_init(&arr, args->pAllocateInfo->arg_count,                       \
                             VK_OBJECT_TYPE_##vk_type, sizeof(struct vkr_##vkr_type),    \
                             sizeof(Vk##vk_obj), args->p##vk_obj##s)) {                  \
         args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;                                        \
         return;                                                                         \
      }                                                                                  \
                                                                                         \
      vn_replace_##vk_cmd##_args_handle(args);                                           \
      args->ret = vk_cmd(args->device, args->pAllocateInfo, arr.handle_storage);         \
      if (args->ret != VK_SUCCESS) {                                                     \
         object_array_fini(&arr);                                                        \
         return;                                                                         \
      }                                                                                  \
                                                                                         \
      for (uint32_t i = 0; i < arr.count; i++) {                                         \
         struct vkr_##vkr_type *obj = arr.objects[i];                                    \
                                                                                         \
         obj->base.handle.vkr_type = ((Vk##vk_obj *)arr.handle_storage)[i];              \
         obj->device = dev;                                                              \
                                                                                         \
         /* pool objects are tracked by the pool other than the device */                \
         list_add(&obj->base.track_head, &pool->vkr_type##s);                            \
                                                                                         \
         util_hash_table_set_u64(ctx->object_table, obj->base.id, obj);                  \
      }                                                                                  \
                                                                                         \
      arr.objects_stolen = true;                                                         \
      object_array_fini(&arr);                                                           \
   } while (0)

#define RELEASE_TRACKED_OBJECTS(track_list)                                              \
   do {                                                                                  \
      struct vkr_object *obj, *tmp;                                                      \
      LIST_FOR_EACH_ENTRY_SAFE (obj, tmp, track_list, track_head)                        \
         util_hash_table_remove_u64(ctx->object_table, obj->id);                         \
   } while (0)

#define FREE_POOL_OBJECTS(obj, vkr_type, vk_type, vk_cmd, arg_obj, arg_count, arg_pool)  \
   do {                                                                                  \
      struct list_head free_list;                                                        \
                                                                                         \
      list_inithead(&free_list);                                                         \
      for (uint32_t i = 0; i < args->arg_count; i++) {                                   \
         struct vkr_##vkr_type *obj = (struct vkr_##vkr_type *)args->arg_obj[i];         \
         if (!obj)                                                                       \
            continue;                                                                    \
         if (obj->base.type != VK_OBJECT_TYPE_##vk_type) {                               \
            vkr_cs_decoder_set_fatal(&ctx->decoder);                                     \
            return;                                                                      \
         }                                                                               \
                                                                                         \
         list_del(&obj->base.track_head);                                                \
         list_addtail(&obj->base.track_head, &free_list);                                \
      }                                                                                  \
                                                                                         \
      vn_replace_##vk_cmd##_args_handle(args);                                           \
      vk_cmd(args->device, args->arg_pool, args->arg_count, args->arg_obj);              \
                                                                                         \
      RELEASE_TRACKED_OBJECTS(&free_list);                                               \
   } while (0)

#define CREATE_PIPELINES(vk_cmd)                                                         \
   do {                                                                                  \
      struct vkr_device *dev = (struct vkr_device *)args->device;                        \
      if (!dev || dev->base.type != VK_OBJECT_TYPE_DEVICE) {                             \
         vkr_cs_decoder_set_fatal(&ctx->decoder);                                        \
         return;                                                                         \
      }                                                                                  \
                                                                                         \
      struct object_array arr;                                                           \
      if (!object_array_init(&arr, args->createInfoCount, VK_OBJECT_TYPE_PIPELINE,       \
                             sizeof(struct vkr_pipeline), sizeof(VkPipeline),            \
                             args->pPipelines)) {                                        \
         args->ret = VK_ERROR_OUT_OF_HOST_MEMORY;                                        \
         return;                                                                         \
      }                                                                                  \
                                                                                         \
      vn_replace_##vk_cmd##_args_handle(args);                                           \
      args->ret = vk_cmd(args->device, args->pipelineCache, args->createInfoCount,       \
                         args->pCreateInfos, NULL, arr.handle_storage);                  \
      if (args->ret != VK_SUCCESS) {                                                     \
         object_array_fini(&arr);                                                        \
         return;                                                                         \
      }                                                                                  \
                                                                                         \
      for (uint32_t i = 0; i < arr.count; i++) {                                         \
         struct vkr_pipeline *pipeline = arr.objects[i];                                 \
                                                                                         \
         pipeline->base.handle.pipeline = ((VkPipeline *)arr.handle_storage)[i];         \
                                                                                         \
         list_add(&pipeline->base.track_head, &dev->objects);                            \
                                                                                         \
         util_hash_table_set_u64(ctx->object_table, pipeline->base.id, pipeline);        \
      }                                                                                  \
                                                                                         \
      arr.objects_stolen = true;                                                         \
      object_array_fini(&arr);                                                           \
   } while (0)

struct vkr_context;
struct vkr_instance;
struct vkr_physical_device;
struct vkr_device;
struct vkr_queue;
struct vkr_fence;
struct vkr_semaphore;
struct vkr_event;
struct vkr_device_memory;
struct vkr_buffer;
struct vkr_buffer_view;
struct vkr_image;
struct vkr_image_view;
struct vkr_sampler;
struct vkr_sampler_ycbcr_conversion;
struct vkr_descriptor_set_layout;
struct vkr_descriptor_pool;
struct vkr_descriptor_set;
struct vkr_descriptor_update_template;
struct vkr_render_pass;
struct vkr_framebuffer;
struct vkr_query_pool;
struct vkr_shader_module;
struct vkr_pipeline_layout;
struct vkr_pipeline_cache;
struct vkr_pipeline;
struct vkr_command_pool;
struct vkr_command_buffer;

typedef uint64_t vkr_object_id;

enum vkr_debug_flags {
   VKR_DEBUG_VALIDATE = 1 << 0,
};

/* base class for all objects */
struct vkr_object {
   VkObjectType type;
   vkr_object_id id;

   union {
      uint64_t u64;

      VkInstance instance;
      VkPhysicalDevice physical_device;
      VkDevice device;
      VkQueue queue;
      VkCommandBuffer command_buffer;

      VkBuffer buffer;
      VkImage image;
      VkSemaphore semaphore;
      VkFence fence;
      VkDeviceMemory device_memory;
      VkEvent event;
      VkQueryPool query_pool;
      VkBufferView buffer_view;
      VkImageView image_view;
      VkShaderModule shader_module;
      VkPipelineCache pipeline_cache;
      VkPipelineLayout pipeline_layout;
      VkPipeline pipeline;
      VkRenderPass render_pass;
      VkDescriptorSetLayout descriptor_set_layout;
      VkSampler sampler;
      VkDescriptorSet descriptor_set;
      VkDescriptorPool descriptor_pool;
      VkFramebuffer framebuffer;
      VkCommandPool command_pool;
      VkSamplerYcbcrConversion sampler_ycbcr_conversion;
      VkDescriptorUpdateTemplate descriptor_update_template;
   } handle;

   struct list_head track_head;
};

struct object_array {
   uint32_t count;
   void **objects;
   void *handle_storage;

   /* true if the ownership of the objects has been transferred (to
    * vkr_context::object_table)
    */
   bool objects_stolen;
};

extern uint32_t vkr_renderer_flags;
extern uint32_t vkr_debug_flags;

bool
object_array_init(struct object_array *arr,
                  uint32_t count,
                  VkObjectType obj_type,
                  size_t obj_size,
                  size_t handle_size,
                  const void *handles);

void
object_array_fini(struct object_array *arr);

static inline void *
vkr_find_pnext(const void *chain, VkStructureType type)
{
   VkBaseOutStructure *pnext = (VkBaseOutStructure *)chain;
   while (pnext) {
      if (pnext->sType == type)
         return pnext;
      pnext = pnext->pNext;
   }
   return NULL;
}

#endif /* VKR_COMMON_H */
