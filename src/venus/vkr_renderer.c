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
#include "vkr_transport.h"

static const struct debug_named_value vkr_debug_options[] = {
   { "validate", VKR_DEBUG_VALIDATE, "Force enabling the validation layer" },
   DEBUG_NAMED_VALUE_END
};

uint32_t vkr_renderer_flags;
uint32_t vkr_debug_flags;

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
