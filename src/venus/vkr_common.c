/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "vkr_common.h"

#include "vkr_cs.h"

void
object_array_fini(struct object_array *arr)
{
   if (!arr->objects_stolen) {
      for (uint32_t i = 0; i < arr->count; i++)
         free(arr->objects[i]);
   }

   free(arr->objects);
   free(arr->handle_storage);
}

bool
object_array_init(struct object_array *arr,
                  uint32_t count,
                  VkObjectType obj_type,
                  size_t obj_size,
                  size_t handle_size,
                  const void *handles)
{
   arr->count = count;

   arr->objects = malloc(sizeof(*arr->objects) * count);
   if (!arr->objects)
      return false;

   arr->handle_storage = malloc(handle_size * count);
   if (!arr->handle_storage) {
      free(arr->objects);
      return false;
   }

   arr->objects_stolen = false;
   for (uint32_t i = 0; i < count; i++) {
      struct vkr_object *obj = calloc(1, obj_size);
      if (!obj) {
         arr->count = i;
         object_array_fini(arr);
         return false;
      }

      obj->type = obj_type;
      obj->id = vkr_cs_handle_load_id((const void **)((char *)handles + handle_size * i),
                                      obj->type);

      arr->objects[i] = obj;
   }

   return arr;
}
