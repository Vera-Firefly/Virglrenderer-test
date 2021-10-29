/*
 * Copyright 2021 Google LLC
 * SPDX-License-Identifier: MIT
 */

#include "render_common.h"

#include <stdarg.h>
#include <stdio.h>

void
render_log(const char *fmt, ...)
{
   va_list va;

   va_start(va, fmt);

   fprintf(stderr, "render: ");
   vfprintf(stderr, fmt, va);
   fprintf(stderr, "\n");

   va_end(va);
}
