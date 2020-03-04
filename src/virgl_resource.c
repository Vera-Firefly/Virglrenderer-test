/**************************************************************************
 *
 * Copyright (C) 2020 Chromium
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include "virgl_resource.h"

#include <errno.h>
#include <string.h>

#include "util/u_hash_table.h"
#include "util/u_pointer.h"
#include "virgl_util.h"

static struct util_hash_table *virgl_resource_table;
static struct virgl_resource_pipe_callbacks pipe_callbacks;

static void
virgl_resource_destroy_func(void *val)
{
   struct virgl_resource *res = (struct virgl_resource *)val;

   if (res->pipe_resource)
      pipe_callbacks.unref(res->pipe_resource, pipe_callbacks.data);

   free(res);
}

int
virgl_resource_table_init(const struct virgl_resource_pipe_callbacks *callbacks)
{
   virgl_resource_table = util_hash_table_create(hash_func_u32,
                                                 compare_func,
                                                 virgl_resource_destroy_func);
   if (!virgl_resource_table)
      return ENOMEM;

   pipe_callbacks = *callbacks;

   return 0;
}

void
virgl_resource_table_cleanup(void)
{
   util_hash_table_destroy(virgl_resource_table);
   memset(&pipe_callbacks, 0, sizeof(pipe_callbacks));
}

void
virgl_resource_table_reset(void)
{
   util_hash_table_clear(virgl_resource_table);
}

int
virgl_resource_create_from_pipe(uint32_t res_id, struct pipe_resource *pres)
{
   struct virgl_resource *res;
   enum pipe_error err;

   res = calloc(1, sizeof(*res));
   if (!res)
      return ENOMEM;

   err = util_hash_table_set(virgl_resource_table,
                             uintptr_to_pointer(res_id),
                             res);
   if (err != PIPE_OK) {
      free(res);
      return ENOMEM;
   }

   res->res_id = res_id;
   /* take ownership */
   res->pipe_resource = pres;

   return 0;
}

void
virgl_resource_remove(uint32_t res_id)
{
   util_hash_table_remove(virgl_resource_table, uintptr_to_pointer(res_id));
}

struct virgl_resource *virgl_resource_lookup(uint32_t res_id)
{
   return util_hash_table_get(virgl_resource_table,
                              uintptr_to_pointer(res_id));
}