# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
from functools import partial
from subprocess import Popen
from sys import platform

import cmake
import fs_utils
import build

VK_ROOT = fs_utils.build_dir / 'vulkan-headers'
VK_INCLUDE = VK_ROOT / 'include'
TAG = 'vulkan-sdk-1.3.290.0' # must match TAG in vk_bootstrap.py

build.base.compile_args += ['-I', VK_INCLUDE]

def hook_recipe(recipe):
  recipe.add_step(
      partial(Popen, ['git', '-c', 'advice.detachedHead=false', 'clone', '--depth', '1', '--branch', TAG, 'https://github.com/KhronosGroup/Vulkan-Headers.git', VK_ROOT]),
      outputs=[VK_INCLUDE],
      inputs=[],
      desc = 'Downloading Vulkan-Headers',
      shortcut='get vulkan-headers')

def hook_plan(srcs, objs, bins, recipe):
  for obj in objs:
    if 'vulkan/vulkan.h' in obj.source.system_includes:
      obj.deps.add(VK_INCLUDE)
