# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

import functools
from fnmatch import fnmatch
from sys import platform

import build
import fs_utils
import make

if platform == 'linux':
  TARGET_CPP = fs_utils.src_dir / 'mem_meld_target.cpp'
  TARGET_BIN = build.BASE / 'mem_meld_target'

  def hook_recipe(recipe):
    recipe.add_step(
        functools.partial(make.Popen, [build.compiler[0], '-std=gnu++26', '-O1', '-g', TARGET_CPP, '-o', TARGET_BIN]),
        outputs=[TARGET_BIN],
        inputs=[TARGET_CPP, __file__],
        desc='Building the MemMeld test target for the host',
        shortcut='link mem_meld_target')
    recipe.generated.add(str(TARGET_BIN))

  def hook_srcs(srcs, recipe):
    del srcs[str(TARGET_CPP)]

  def hook_final(srcs, objs, bins, recipe):
    tests = {str(bin.path) for bin in bins if fnmatch(bin.path.name, 'mem_meld*_test')}
    for step in recipe.steps:
      if step.outputs & tests:
        step.inputs.add(str(TARGET_BIN))
