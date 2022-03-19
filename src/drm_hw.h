/*
 * Copyright 2022 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef DRM_HW_H_
#define DRM_HW_H_

struct virgl_renderer_capset_drm {
   uint32_t wire_format_version;
   /* Underlying drm device version: */
   uint32_t version_major;
   uint32_t version_minor;
   uint32_t version_patchlevel;
   uint32_t context_type;
   uint32_t pad;
   union {
   } u;
};

#endif /* DRM_HW_H_ */
