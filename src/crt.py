# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

import functools
from sys import platform

import build
import fs_utils
import make

if platform == 'linux':
  HELPER_C = fs_utils.src_dir / 'crt_helper.c'
  HELPER = fs_utils.generated_dir / 'crt_helper'

  def hook_recipe(recipe):
    recipe.add_step(
        functools.partial(make.Popen, [build.compiler_c[0], '-pie', '-fPIC', '-O2', '-nostartfiles', HELPER_C,
                                       '-o', HELPER, '-Wl,--no-as-needed', '-l:libdl.so.2', '-l:libpthread.so.0', '-Wl,-e,_start', '-Wl,-soname,crt_helper', '-Wl,-z,norelro', '-Wl,--export-dynamic-symbol=_Unwind_*', '-Wl,--export-dynamic-symbol=__gcc_personality_v0', '-s']),
        outputs=[HELPER],
        inputs=[HELPER_C, __file__],
        desc='Building the crt helper for the host',
        shortcut='crt-helper')
    recipe.generated.add(str(HELPER))
