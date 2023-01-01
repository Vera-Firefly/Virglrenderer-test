/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_common.h"

#include "util/os_file.h"
#include "util/u_debug.h"
#include "venus-protocol/vn_protocol_renderer_info.h"
#include "virgl_context.h"
#include "virgl_resource.h"
#include "virglrenderer_hw.h"

#include "vkr_allocator.h"

static const struct debug_named_value vkr_debug_options[] = {
   { "validate", VKR_DEBUG_VALIDATE, "Force enabling the validation layer" },
   DEBUG_NAMED_VALUE_END
};

const struct virgl_renderer_callbacks *vkr_renderer_cbs;
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
         vkr_extension_get_spec_version("VK_EXT_command_serialization");
      c->vk_mesa_venus_protocol_spec_version =
         vkr_extension_get_spec_version("VK_MESA_venus_protocol");
      /* After https://gitlab.freedesktop.org/virgl/virglrenderer/-/merge_requests/688,
       * this flag is used to indicate render server config, and will be needed until drm
       * virtio-gpu blob mem gets fixed to attach_resource before resource_map.
       */
      c->supports_blob_id_0 = (bool)(vkr_renderer_flags & VKR_RENDERER_RENDER_SERVER);

      uint32_t ext_mask[VN_INFO_EXTENSION_MAX_NUMBER / 32 + 1] = { 0 };
      vn_info_extension_mask_init(ext_mask);

      static_assert(sizeof(ext_mask) <= sizeof(c->vk_extension_mask1),
                    "Time to extend venus capset with vk_extension_mask2");
      memcpy(c->vk_extension_mask1, ext_mask, sizeof(ext_mask));

      /* set bit 0 to enable the extension mask(s) */
      assert(!(c->vk_extension_mask1[0] & 0x1u));
      c->vk_extension_mask1[0] |= 0x1u;

      c->allow_vk_wait_syncs = 1;
      c->supports_multiple_timelines = 1;
   }

   return sizeof(*c);
}

int
vkr_renderer_init(uint32_t flags)
{
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

bool
vkr_renderer_init2(uint32_t flags,
                   virgl_debug_callback_type debug_cb,
                   const struct virgl_renderer_callbacks *cbs)
{
   TRACE_INIT();
   TRACE_FUNC();

   static const uint32_t required_flags =
      VKR_RENDERER_THREAD_SYNC | VKR_RENDERER_ASYNC_FENCE_CB;
   if ((flags & required_flags) != required_flags)
      return false;

   virgl_log_set_logger(debug_cb);

   vkr_renderer_flags = flags;
   vkr_renderer_cbs = cbs;
   vkr_debug_flags = debug_get_flags_option("VKR_DEBUG", vkr_debug_options, 0);

   int ret = virgl_resource_table_init(NULL);
   if (ret)
      return false;

   ret = virgl_context_table_init();
   if (ret) {
      virgl_resource_table_cleanup();
      return false;
   }

   return true;
}

void
vkr_renderer_fini2(void)
{
   virgl_context_table_cleanup();
   virgl_resource_table_cleanup();

   vkr_debug_flags = 0;
   vkr_renderer_cbs = NULL;
   vkr_renderer_flags = 0;

   vkr_allocator_fini();
}

static void
vkr_renderer_retire_fence(struct virgl_context *ctx, uint32_t ring_idx, uint64_t fence_id)
{
   TRACE_FUNC();

   vkr_renderer_cbs->write_context_fence(NULL, ctx->ctx_id, ring_idx, fence_id);
}

bool
vkr_renderer_create_context(uint32_t ctx_id,
                            uint32_t ctx_flags,
                            uint32_t nlen,
                            const char *name)
{
   TRACE_FUNC();

   assert(ctx_id);
   assert(!(ctx_flags & ~VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK));

   if ((ctx_flags & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK) !=
       VIRGL_RENDERER_CAPSET_VENUS)
      return false;

   /* duplicate ctx creation between server and vkr is invalid */
   struct virgl_context *ctx = virgl_context_lookup(ctx_id);
   if (ctx)
      return false;

   ctx = vkr_context_create(nlen, name);
   if (!ctx)
      return false;

   ctx->ctx_id = ctx_id;
   ctx->capset_id = VIRGL_RENDERER_CAPSET_VENUS;
   ctx->fence_retire = vkr_renderer_retire_fence;

   int ret = virgl_context_add(ctx);
   if (ret) {
      ctx->destroy(ctx);
      return false;
   }

   return true;
}

void
vkr_renderer_destroy_context(uint32_t ctx_id)
{
   TRACE_FUNC();

   virgl_context_remove(ctx_id);
}

bool
vkr_renderer_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size)
{
   TRACE_FUNC();

   struct virgl_context *ctx = virgl_context_lookup(ctx_id);
   if (!ctx)
      return false;

   return !ctx->submit_cmd(ctx, cmd, size);
}

bool
vkr_renderer_submit_fence(uint32_t ctx_id,
                          uint32_t flags,
                          uint64_t ring_idx,
                          uint64_t fence_id)
{
   TRACE_FUNC();

   struct virgl_context *ctx = virgl_context_lookup(ctx_id);
   if (!ctx)
      return false;

   assert(vkr_renderer_cbs->write_context_fence);
   return !ctx->submit_fence(ctx, flags, ring_idx, fence_id);
}

bool
vkr_renderer_create_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             uint32_t blob_flags,
                             uint64_t blob_id,
                             uint64_t blob_size,
                             enum virgl_resource_fd_type *out_fd_type,
                             int *out_res_fd,
                             uint32_t *out_map_info,
                             struct virgl_resource_vulkan_info *out_vulkan_info)
{
   TRACE_FUNC();

   assert(res_id);
   assert(!virgl_resource_lookup(res_id));
   assert(blob_size);

   struct virgl_context *ctx = virgl_context_lookup(ctx_id);
   if (!ctx)
      return false;

   struct virgl_context_blob blob;
   int ret = ctx->get_blob(ctx, res_id, blob_id, blob_size, blob_flags, &blob);
   if (ret)
      return false;

   assert(blob.type == VIRGL_RESOURCE_FD_SHM || blob.type == VIRGL_RESOURCE_FD_DMABUF ||
          blob.type == VIRGL_RESOURCE_FD_OPAQUE);

   struct virgl_resource *res = virgl_resource_create_from_fd(
      res_id, blob.type, blob.u.fd, NULL, 0, &blob.vulkan_info);
   if (!res) {
      close(blob.u.fd);
      return false;
   }

   res->map_info = blob.map_info;
   res->map_size = blob_size;

   int res_fd = os_dupfd_cloexec(res->fd);
   if (res_fd < 0) {
      virgl_resource_remove(res_id);
      return false;
   }

   /*
    * RENDER_CONTEXT_OP_CREATE_RESOURCE implies attach and proxy will not send
    * RENDER_CONTEXT_OP_IMPORT_RESOURCE to attach the resource again.
    */
   ctx->attach_resource(ctx, res);

   *out_fd_type = blob.type;
   *out_res_fd = res_fd;
   *out_map_info = blob.map_info;

   if (blob.type == VIRGL_RESOURCE_FD_OPAQUE) {
      assert(out_vulkan_info);
      *out_vulkan_info = blob.vulkan_info;
   }

   return true;
}

bool
vkr_renderer_import_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             enum virgl_resource_fd_type fd_type,
                             int fd,
                             uint64_t size)
{
   TRACE_FUNC();

   assert(res_id);
   assert(!virgl_resource_lookup(res_id));
   assert(fd_type == VIRGL_RESOURCE_FD_SHM || fd_type == VIRGL_RESOURCE_FD_DMABUF ||
          fd_type == VIRGL_RESOURCE_FD_OPAQUE);
   assert(fd >= 0);
   assert(size);

   struct virgl_context *ctx = virgl_context_lookup(ctx_id);
   if (!ctx)
      return false;

   struct virgl_resource *res =
      virgl_resource_create_from_fd(res_id, fd_type, fd, NULL, 0, NULL);
   if (!res)
      return false;

   res->map_info = 0;
   res->map_size = size;

   ctx->attach_resource(ctx, res);

   return true;
}

static bool
vkr_renderer_detach_resource(struct virgl_context *ctx, void *data)
{
   struct virgl_resource *res = data;
   ctx->detach_resource(ctx, res);
   return true;
}

void
vkr_renderer_destroy_resource(UNUSED uint32_t ctx_id, uint32_t res_id)
{
   struct virgl_resource *res = virgl_resource_lookup(res_id);
   struct virgl_context_foreach_args args;

   if (!res)
      return;

   args.callback = vkr_renderer_detach_resource;
   args.data = res;
   virgl_context_foreach(&args);

   virgl_resource_remove(res_id);
}
