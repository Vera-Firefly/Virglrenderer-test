/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_query_pool.h"

#include "venus-protocol/vn_protocol_renderer_query_pool.h"

#include "vkr_context.h"
#include "vkr_device.h"

static void
vkr_dispatch_vkCreateQueryPool(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkCreateQueryPool *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(pool, query_pool, QUERY_POOL, vkCreateQueryPool, pQueryPool);

   vkr_device_add_object(ctx, &pool->base);
}

static void
vkr_dispatch_vkDestroyQueryPool(struct vn_dispatch_context *dispatch,
                                struct vn_command_vkDestroyQueryPool *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(pool, query_pool, QUERY_POOL, vkDestroyQueryPool, queryPool);

   vkr_device_remove_object(ctx, &pool->base);
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

void
vkr_context_init_query_pool_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateQueryPool = vkr_dispatch_vkCreateQueryPool;
   dispatch->dispatch_vkDestroyQueryPool = vkr_dispatch_vkDestroyQueryPool;
   dispatch->dispatch_vkGetQueryPoolResults = vkr_dispatch_vkGetQueryPoolResults;
   dispatch->dispatch_vkResetQueryPool = vkr_dispatch_vkResetQueryPool;
}
