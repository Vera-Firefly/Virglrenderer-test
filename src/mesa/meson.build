# Copyright 2021 Google LLC
# SPDX-License-Identifier: MIT

inc_mesa = include_directories('.')

files_mesa = files(
)

deps_mesa = [
]

libmesa = static_library(
  'mesa',
  files_mesa,
  include_directories: inc_mesa,
  dependencies: deps_mesa,
)

mesa_dep = declare_dependency(
  link_with: libmesa,
  include_directories: inc_mesa,
  dependencies: deps_mesa,
)
