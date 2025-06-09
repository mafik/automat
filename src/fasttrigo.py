# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
from functools import partial
from make import Popen
from sys import platform

import cmake
import fs_utils
import build

FASTTRIGO_ROOT = fs_utils.third_party_dir / 'FastTrigo'
FASTTRIGO_INCLUDE = FASTTRIGO_ROOT

build.compile_args += ['-I', FASTTRIGO_INCLUDE]

object_path = build.BASE / 'FastTrigo' / 'fasttrigo.o'

def hook_recipe(recipe):
  object_path.parent.mkdir(parents=True, exist_ok=True)
  outputs = [str(object_path)]
  recipe.add_step(
      partial(Popen, [build.compiler] + build.CXXFLAGS() +
                        ['-c', str(FASTTRIGO_ROOT / 'fasttrigo.cpp'), '-o'] + outputs),
      outputs=outputs,
      inputs=[FASTTRIGO_ROOT / 'fasttrigo.cpp'],
      desc='Building FastTrigo',
      shortcut='build fasttrigo')

fasttrigo_bins = set()

def hook_plan(srcs, objs, bins, recipe):
  fasttrigo_objs = set()
  for obj in objs:
    if 'fasttrigo.h' in obj.source.system_includes:
      fasttrigo_objs.add(obj)

  for bin in bins:
    if fasttrigo_objs.intersection(bin.objects):
      bin.link_args.append(str(object_path))
      fasttrigo_bins.add(bin)

def hook_final(srcs, objs, bins, recipe):
  for step in recipe.steps:
    needs_fasttrigo = False
    for bin in fasttrigo_bins:
      if str(bin.path) in step.outputs:
        needs_fasttrigo = True
        break
    
    if needs_fasttrigo:
      step.inputs.add(str(object_path))
