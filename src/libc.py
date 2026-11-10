# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

import functools
from sys import platform

import build
import fs_utils
import make

if platform == 'linux':
  HELPER_C = fs_utils.src_dir / 'libc_helper.c'
  HELPER = fs_utils.generated_dir / 'libc_helper'

  def hook_recipe(recipe):
    recipe.add_step(
        functools.partial(make.Popen, [build.compiler_c, '-pie', '-fPIC', '-O2', '-nostartfiles', HELPER_C,
                                       '-o', HELPER, '-Wl,--no-as-needed', '-l:libdl.so.2', '-Wl,-e,_start', '-s']),
        outputs=[HELPER],
        inputs=[HELPER_C, __file__],
        desc='Building the libc helper for the host',
        shortcut='libc-helper')
    recipe.generated.add(str(HELPER))
