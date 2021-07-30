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

typedef uint64_t vkr_object_id;

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

#endif /* VKR_COMMON_H */
