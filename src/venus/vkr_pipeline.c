/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_pipeline.h"

#include "vkr_pipeline_gen.h"

static void
vkr_dispatch_vkCreateShaderModule(struct vn_dispatch_context *dispatch,
                                  struct vn_command_vkCreateShaderModule *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(mod, shader_module, SHADER_MODULE, vkCreateShaderModule, pShaderModule);

   vkr_device_add_object(ctx, &mod->base);
}

static void
vkr_dispatch_vkDestroyShaderModule(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkDestroyShaderModule *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(mod, shader_module, SHADER_MODULE, vkDestroyShaderModule, shaderModule);

   vkr_device_remove_object(ctx, &mod->base);
}

static void
vkr_dispatch_vkCreatePipelineLayout(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkCreatePipelineLayout *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(layout, pipeline_layout, PIPELINE_LAYOUT, vkCreatePipelineLayout,
                 pPipelineLayout);

   vkr_device_add_object(ctx, &layout->base);
}

static void
vkr_dispatch_vkDestroyPipelineLayout(struct vn_dispatch_context *dispatch,
                                     struct vn_command_vkDestroyPipelineLayout *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(layout, pipeline_layout, PIPELINE_LAYOUT, vkDestroyPipelineLayout,
                  pipelineLayout);

   vkr_device_remove_object(ctx, &layout->base);
}

static void
vkr_dispatch_vkCreatePipelineCache(struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkCreatePipelineCache *args)
{
   struct vkr_context *ctx = dispatch->data;

   CREATE_OBJECT(cache, pipeline_cache, PIPELINE_CACHE, vkCreatePipelineCache,
                 pPipelineCache);

   vkr_device_add_object(ctx, &cache->base);
}

static void
vkr_dispatch_vkDestroyPipelineCache(struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkDestroyPipelineCache *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(cache, pipeline_cache, PIPELINE_CACHE, vkDestroyPipelineCache,
                  pipelineCache);

   vkr_device_remove_object(ctx, &cache->base);
}

static void
vkr_dispatch_vkGetPipelineCacheData(UNUSED struct vn_dispatch_context *dispatch,
                                    struct vn_command_vkGetPipelineCacheData *args)
{
   vn_replace_vkGetPipelineCacheData_args_handle(args);
   args->ret = vkGetPipelineCacheData(args->device, args->pipelineCache, args->pDataSize,
                                      args->pData);
}

static void
vkr_dispatch_vkMergePipelineCaches(UNUSED struct vn_dispatch_context *dispatch,
                                   struct vn_command_vkMergePipelineCaches *args)
{
   vn_replace_vkMergePipelineCaches_args_handle(args);
   args->ret = vkMergePipelineCaches(args->device, args->dstCache, args->srcCacheCount,
                                     args->pSrcCaches);
}

static void
vkr_dispatch_vkCreateGraphicsPipelines(struct vn_dispatch_context *dispatch,
                                       struct vn_command_vkCreateGraphicsPipelines *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct object_array arr;

   if (vkr_graphics_pipeline_create_array(ctx, args, &arr) != VK_SUCCESS)
      return;

   vkr_pipeline_add_array(ctx, dev, &arr);
}

static void
vkr_dispatch_vkCreateComputePipelines(struct vn_dispatch_context *dispatch,
                                      struct vn_command_vkCreateComputePipelines *args)
{
   struct vkr_context *ctx = dispatch->data;
   struct vkr_device *dev = vkr_device_from_handle(args->device);
   struct object_array arr;

   if (vkr_compute_pipeline_create_array(ctx, args, &arr) != VK_SUCCESS)
      return;

   vkr_pipeline_add_array(ctx, dev, &arr);
}

static void
vkr_dispatch_vkDestroyPipeline(struct vn_dispatch_context *dispatch,
                               struct vn_command_vkDestroyPipeline *args)
{
   struct vkr_context *ctx = dispatch->data;

   DESTROY_OBJECT(pipeline, pipeline, PIPELINE, vkDestroyPipeline, pipeline);

   vkr_device_remove_object(ctx, &pipeline->base);
}

void
vkr_context_init_shader_module_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateShaderModule = vkr_dispatch_vkCreateShaderModule;
   dispatch->dispatch_vkDestroyShaderModule = vkr_dispatch_vkDestroyShaderModule;
}

void
vkr_context_init_pipeline_layout_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreatePipelineLayout = vkr_dispatch_vkCreatePipelineLayout;
   dispatch->dispatch_vkDestroyPipelineLayout = vkr_dispatch_vkDestroyPipelineLayout;
}

void
vkr_context_init_pipeline_cache_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreatePipelineCache = vkr_dispatch_vkCreatePipelineCache;
   dispatch->dispatch_vkDestroyPipelineCache = vkr_dispatch_vkDestroyPipelineCache;
   dispatch->dispatch_vkGetPipelineCacheData = vkr_dispatch_vkGetPipelineCacheData;
   dispatch->dispatch_vkMergePipelineCaches = vkr_dispatch_vkMergePipelineCaches;
}

void
vkr_context_init_pipeline_dispatch(struct vkr_context *ctx)
{
   struct vn_dispatch_context *dispatch = &ctx->dispatch;

   dispatch->dispatch_vkCreateGraphicsPipelines = vkr_dispatch_vkCreateGraphicsPipelines;
   dispatch->dispatch_vkCreateComputePipelines = vkr_dispatch_vkCreateComputePipelines;
   dispatch->dispatch_vkDestroyPipeline = vkr_dispatch_vkDestroyPipeline;
}
