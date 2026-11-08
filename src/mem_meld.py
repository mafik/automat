# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

from fnmatch import fnmatch
from sys import platform

import build

if platform == 'linux':
  TARGET_BIN = build.BASE / 'mem_meld_target'

  def hook_final(srcs, objs, bins, recipe):
    tests = {str(bin.path) for bin in bins if fnmatch(bin.path.name, 'mem_meld*_test')}
    for step in recipe.steps:
      if step.outputs & tests:
        step.inputs.add(str(TARGET_BIN))
