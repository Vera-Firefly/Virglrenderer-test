/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "render_state.h"

#include <inttypes.h>

#include "virglrenderer.h"

#include "render_context.h"

struct render_state render_state_internal = {
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   .struct_mutex = _MTX_INITIALIZER_NP,
   .dispatch_mutex = _MTX_INITIALIZER_NP,
#endif
   .init_count = 0,
};

static struct render_state *
render_state_lock_struct(void)
{
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   mtx_lock(&render_state_internal.struct_mutex);
#endif
   return &render_state_internal;
}

static void
render_state_unlock_struct(void)
{
#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   mtx_unlock(&render_state_internal.struct_mutex);
#endif
}

static struct render_context *
render_state_lookup_context(uint32_t ctx_id)
{
   const struct render_state *virgl = render_state_lock_struct();
   struct render_context *ctx = NULL;

#ifdef ENABLE_RENDER_SERVER_WORKER_THREAD
   list_for_each_entry (struct render_context, iter, &virgl->contexts, head) {
      if (iter->ctx_id == ctx_id) {
         ctx = iter;
         break;
      }
   }
#else
   assert(list_is_singular(&virgl->contexts));
   ctx = list_first_entry(&virgl->contexts, struct render_context, head);
   assert(ctx->ctx_id == ctx_id);
   (void)ctx_id;
#endif

   render_state_unlock_struct();

   return ctx;
}

static void
render_state_debug_callback(const char *fmt, va_list ap)
{
   char buf[1024];
   vsnprintf(buf, sizeof(buf), fmt, ap);
   render_log(buf);
}

static void
render_state_cb_write_context_fence(UNUSED void *cookie,
                                    uint32_t ctx_id,
                                    uint32_t ring_idx,
                                    uint64_t fence_id)
{
   struct render_context *ctx = render_state_lookup_context(ctx_id);
   assert(ctx);

   const uint32_t seqno = (uint32_t)fence_id;
   render_context_update_timeline(ctx, ring_idx, seqno);
}

static const struct virgl_renderer_callbacks render_state_cbs = {
   .version = VIRGL_RENDERER_CALLBACKS_VERSION,
   .write_context_fence = render_state_cb_write_context_fence,
};

void
render_state_add_context(struct render_context *ctx)
{
   struct render_state *virgl = render_state_lock_struct();
   list_addtail(&ctx->head, &virgl->contexts);
   render_state_unlock_struct();
}

void
render_state_remove_context(struct render_context *ctx)
{
   render_state_lock_struct();
   list_del(&ctx->head);
   render_state_unlock_struct();
}

void
render_state_fini(void)
{
   struct render_state *virgl = render_state_lock_struct();

   if (virgl->init_count) {
      virgl->init_count--;
      if (!virgl->init_count) {
         render_state_lock_dispatch();
         virgl_renderer_cleanup(virgl);
         render_state_unlock_dispatch();
      }
   }

   render_state_unlock_struct();
}

bool
render_state_init(uint32_t init_flags)
{
   /* we only care if virgl and/or venus are enabled */
   init_flags &= VIRGL_RENDERER_VENUS | VIRGL_RENDERER_NO_VIRGL;

   /* always use sync thread and async fence cb for low latency */
   init_flags |= VIRGL_RENDERER_THREAD_SYNC | VIRGL_RENDERER_ASYNC_FENCE_CB |
                 VIRGL_RENDERER_USE_EXTERNAL_BLOB;

   struct render_state *virgl = render_state_lock_struct();

   if (virgl->init_count) {
      if (virgl->init_flags != init_flags) {
         render_log("failed to re-initialize with flags 0x%x", init_flags);
         goto fail;
      }
   } else {
      render_state_lock_dispatch();
      virgl_set_debug_callback(render_state_debug_callback);
      int ret = virgl_renderer_init(virgl, init_flags,
                                    (struct virgl_renderer_callbacks *)&render_state_cbs);
      render_state_unlock_dispatch();
      if (ret) {
         render_log("failed to initialize virglrenderer");
         goto fail;
      }

      list_inithead(&virgl->contexts);
      virgl->init_flags = init_flags;
   }

   virgl->init_count++;

   render_state_unlock_struct();

   return true;

fail:
   render_state_unlock_struct();
   return false;
}

bool
render_state_create_context(uint32_t ctx_id,
                            uint32_t flags,
                            uint32_t name_len,
                            const char *name)
{
   int ret = virgl_renderer_context_create_with_flags(ctx_id, flags, name_len, name);
   if (ret) {
      render_log("failed to create context %u with flags %u (%d)", ctx_id, flags, ret);
      return false;
   }

   return true;
}

void
render_state_destroy_context(uint32_t ctx_id)
{
   render_state_lock_dispatch();
   /* destroy the context first to join its sync threads and ring threads */
   virgl_renderer_context_destroy(ctx_id);
   render_state_unlock_dispatch();
}

bool
render_state_submit_cmd(uint32_t ctx_id, void *cmd, uint32_t size)
{
   int ret = virgl_renderer_submit_cmd(cmd, ctx_id, size / sizeof(uint32_t));
   if (ret) {
      render_log("failed to submit cmd: ctx_id %u size %u ret(%d)", ctx_id, size, ret);
      return false;
   }

   return true;
}

bool
render_state_submit_fence(uint32_t ctx_id,
                          uint32_t flags,
                          uint64_t queue_id,
                          uint64_t fence_id)
{

   int ret = virgl_renderer_context_create_fence(ctx_id, flags, queue_id, fence_id);
   if (ret) {
      render_log(
         "failed to create context fence: ctx_id %u with flags %u queue_id %" PRIu64
         " fence_id %" PRIu64 " ret(%d)",
         ctx_id, flags, queue_id, fence_id, ret);
      return false;
   }

   return true;
}

bool
render_state_create_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             uint32_t blob_flags,
                             uint64_t blob_id,
                             uint64_t blob_size,
                             enum virgl_resource_fd_type *out_fd_type,
                             int *out_res_fd,
                             uint32_t *out_map_info)
{
   const struct virgl_renderer_resource_create_blob_args blob_args = {
      .res_handle = res_id,
      .ctx_id = ctx_id,
      .blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D,
      .blob_flags = blob_flags,
      .blob_id = blob_id,
      .size = blob_size,
   };
   int ret = virgl_renderer_resource_create_blob(&blob_args);
   if (ret) {
      render_log("failed to create blob resource");
      return false;
   }

   uint32_t map_info;
   ret = virgl_renderer_resource_get_map_info(res_id, &map_info);
   if (ret) {
      /* properly set map_info when the resource has no map cache info */
      map_info = VIRGL_RENDERER_MAP_CACHE_NONE;
   }

   uint32_t fd_type;
   int res_fd;
   ret = virgl_renderer_resource_export_blob(res_id, &fd_type, &res_fd);
   if (ret) {
      virgl_renderer_resource_unref(res_id);
      return false;
   }

   /* RENDER_CONTEXT_OP_CREATE_RESOURCE implies attach and proxy will not send
    * RENDER_CONTEXT_OP_IMPORT_RESOURCE to attach the resource again.
    */
   virgl_renderer_ctx_attach_resource(ctx_id, res_id);

   switch (fd_type) {
   case VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF:
      *out_fd_type = VIRGL_RESOURCE_FD_DMABUF;
      break;
   case VIRGL_RENDERER_BLOB_FD_TYPE_OPAQUE:
      *out_fd_type = VIRGL_RESOURCE_FD_OPAQUE;
      break;
   case VIRGL_RENDERER_BLOB_FD_TYPE_SHM:
      *out_fd_type = VIRGL_RESOURCE_FD_SHM;
      break;
   default:
      *out_fd_type = 0;
   }

   *out_map_info = map_info;
   *out_res_fd = res_fd;

   return true;
}

bool
render_state_import_resource(uint32_t ctx_id,
                             uint32_t res_id,
                             enum virgl_resource_fd_type fd_type,
                             int fd,
                             uint64_t size)
{
   if (fd_type == VIRGL_RESOURCE_FD_INVALID || !size) {
      render_log("failed to attach invalid resource %d", res_id);
      return false;
   }

   uint32_t blob_fd_type;
   switch (fd_type) {
   case VIRGL_RESOURCE_FD_DMABUF:
      blob_fd_type = VIRGL_RENDERER_BLOB_FD_TYPE_DMABUF;
      break;
   case VIRGL_RESOURCE_FD_OPAQUE:
      blob_fd_type = VIRGL_RENDERER_BLOB_FD_TYPE_OPAQUE;
      break;
   case VIRGL_RESOURCE_FD_SHM:
      blob_fd_type = VIRGL_RENDERER_BLOB_FD_TYPE_SHM;
      break;
   default:
      render_log("unknown fd_type %d", fd_type);
      return false;
   }

   const struct virgl_renderer_resource_import_blob_args import_args = {
      .res_handle = res_id,
      .blob_mem = VIRGL_RENDERER_BLOB_MEM_HOST3D,
      .fd_type = blob_fd_type,
      .fd = fd,
      .size = size,
   };
   int ret = virgl_renderer_resource_import_blob(&import_args);
   if (ret) {
      render_log("failed to import blob resource %u (%d)", res_id, ret);
      return false;
   }

   virgl_renderer_ctx_attach_resource(ctx_id, res_id);

   return true;
}

void
render_state_destroy_resource(UNUSED uint32_t ctx_id, uint32_t res_id)
{
   virgl_renderer_resource_unref(res_id);
}
