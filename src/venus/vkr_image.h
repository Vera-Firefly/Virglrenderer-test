/*
 * Copyright 2020 Google LLC
 * SPDX-License-Identifier: MIT
 */

#ifndef VKR_IMAGE_H
#define VKR_IMAGE_H

#include "vkr_common.h"

struct vkr_image {
   struct vkr_object base;
};

struct vkr_image_view {
   struct vkr_object base;
};

struct vkr_sampler {
   struct vkr_object base;
};

struct vkr_sampler_ycbcr_conversion {
   struct vkr_object base;
};

#endif /* VKR_IMAGE_H */
