/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_render_pass.h"

#include "venus-protocol/vn_protocol_renderer_framebuffer.h"
#include "venus-protocol/vn_protocol_renderer_render_pass.h"

#include "vkr_context.h"
#include "vkr_device.h"

static void
vkr_dispatch_vkCreateRenderPass(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkCreateRenderPass *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(pass, render_pass, RENDER_PASS, vkCreateRenderPass, pRenderPass);

   vkr_device_add_object(ctx, &pass->base);
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

   vkr_device_add_object(ctx, &pass->base);
}

static void
vkr_dispatch_vkDestroyRenderPass(struct vn_dispatch_context *dispatch,
                                 struct vn_command_vkDestroyRenderPass *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(pass, render_pass, RENDER_PASS, vkDestroyRenderPass, renderPass);

   vkr_device_remove_object(ctx, &pass->base);
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

   vkr_device_add_object(ctx, &fb->base);
}

static void
vkr_dispatch_vkDestroyFramebuffer(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkDestroyFramebuffer *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(fb, framebuffer, FRAMEBUFFER, vkDestroyFramebuffer, framebuffer);

   vkr_device_remove_object(ctx, &fb->base);
}

void
vkr_context_init_render_pass_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateRenderPass = vkr_dispatch_vkCreateRenderPass;
   dispatch->dispatch_vkCreateRenderPass2 = vkr_dispatch_vkCreateRenderPass2;
   dispatch->dispatch_vkDestroyRenderPass = vkr_dispatch_vkDestroyRenderPass;
   dispatch->dispatch_vkGetRenderAreaGranularity =
      vkr_dispatch_vkGetRenderAreaGranularity;
}

void
vkr_context_init_framebuffer_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateFramebuffer = vkr_dispatch_vkCreateFramebuffer;
   dispatch->dispatch_vkDestroyFramebuffer = vkr_dispatch_vkDestroyFramebuffer;
}
