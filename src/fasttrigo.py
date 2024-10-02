# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
from functools import partial
from subprocess import Popen
from sys import platform

import cmake
import fs_utils
import build

FASTTRIGO_ROOT = fs_utils.third_party_dir / 'FastTrigo'
FASTTRIGO_INCLUDE = FASTTRIGO_ROOT

build.base.compile_args += ['-I', FASTTRIGO_INCLUDE]

def get_object_path(build_type : build.BuildType):
  return fs_utils.build_dir / 'FastTrigo' / build_type.name / 'fasttrigo.o'

def hook_recipe(recipe):
  for build_type in build.types:
    object_path = get_object_path(build_type)
    object_path.parent.mkdir(parents=True, exist_ok=True)
    outputs = [str(object_path)]
    recipe.add_step(
        partial(Popen, [build.compiler] + build_type.CXXFLAGS() +
                          ['-c', str(FASTTRIGO_ROOT / 'fasttrigo.cpp'), '-o'] + outputs),
        outputs=outputs,
        inputs=[FASTTRIGO_ROOT / 'fasttrigo.cpp'],
        desc=f'Building FastTrigo {build_type}'.strip(),
        shortcut=f'build fasttrigo {build_type}'.strip())

fasttrigo_bins = set()

def hook_plan(srcs, objs, bins, recipe):
  fasttrigo_objs = set()
  for obj in objs:
    if 'fasttrigo.h' in obj.source.system_includes:
      fasttrigo_objs.add(obj)

  for bin in bins:
    if fasttrigo_objs.intersection(bin.objects):
      bin.link_args.append(str(get_object_path(bin.build_type)))
      fasttrigo_bins.add(bin)

def hook_final(srcs, objs, bins, recipe):
  for step in recipe.steps:
    needs_fasttrigo = False
    build_type = None
    for bin in fasttrigo_bins:
      if str(bin.path) in step.outputs:
        needs_fasttrigo = True
        build_type = bin.build_type
        break
    
    if needs_fasttrigo:
      step.inputs.add(str(get_object_path(build_type)))
