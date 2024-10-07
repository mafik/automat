# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
from functools import partial
from subprocess import Popen
from sys import platform

import cmake
import fs_utils
import build
import ninja
import git

VK_BOOTSTRAP_ROOT = fs_utils.build_dir / 'vk-bootstrap'
VK_BOOTSTRAP_INCLUDE = VK_BOOTSTRAP_ROOT / 'src'
TAG = 'v1.3.290' # must match TAG in vk_bootstrap.py

build.base.compile_args += ['-I', VK_BOOTSTRAP_INCLUDE]

def get_build_dir(build_type : build.BuildType):
  return VK_BOOTSTRAP_ROOT / 'build' / build_type.name

libname = build.libname('vk-bootstrap')

def hook_recipe(recipe):
  recipe.add_step(
      git.clone('https://github.com/charles-lunarg/vk-bootstrap', VK_BOOTSTRAP_ROOT, TAG),
      outputs=[VK_BOOTSTRAP_ROOT / 'CMakeLists.txt', VK_BOOTSTRAP_INCLUDE],
      inputs=[],
      desc = 'Downloading vk-bootstrap',
      shortcut='get vk-bootstrap')


  for build_type in build.types:
    build_dir = get_build_dir(build_type)
    cmake_args = cmake.CMakeArgs(build_type)
    recipe.add_step(
        partial(Popen, cmake_args +
                          ['-S', VK_BOOTSTRAP_ROOT, '-B', build_dir]),
        outputs=[build_dir / 'build.ninja'],
        inputs=[VK_BOOTSTRAP_ROOT / 'CMakeLists.txt'],
        desc=f'Configuring vk-bootstrap {build_type}'.strip(),
        shortcut=f'configure vk-bootstrap {build_type}'.strip())

    lib_path = build_dir / libname

    recipe.add_step(
        partial(Popen, [ninja.BIN, '-C', str(build_dir)]),
        outputs=[lib_path],
        inputs=[build_dir / 'build.ninja', ninja.BIN],
        desc='Building vk-bootstrap',
        shortcut=f'vk-bootstrap {build_type}'.strip())

vk_bootstrap_bins = set()

def hook_plan(srcs, objs, bins, recipe):
  vk_bootstrap_objs = set()
  for obj in objs:
    if 'VkBootstrap.h' in obj.source.system_includes:
      obj.deps.add(VK_BOOTSTRAP_INCLUDE)
      vk_bootstrap_objs.add(obj)

  for bin in bins:
    if vk_bootstrap_objs.intersection(bin.objects):
      bin.link_args.append('-L' + str(get_build_dir(bin.build_type)))
      vk_bootstrap_bins.add(bin)

def hook_final(srcs, objs, bins, recipe):
  for step in recipe.steps:
    needs_vk_bootstrap = False
    build_type = None
    for bin in vk_bootstrap_bins:
      if str(bin.path) in step.outputs:
        needs_vk_bootstrap = True
        build_type = bin.build_type
        break
    
    if needs_vk_bootstrap:
      step.inputs.add(str(get_build_dir(build_type) / libname))
