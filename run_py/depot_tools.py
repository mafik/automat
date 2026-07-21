# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT
'''Rules for downloading depot_tools'''
import fs_utils
import git
import build
import os
import os.path
from functools import partial
from make import Popen
from sys import platform

def script_name(stem):
  if platform == 'win32':
    return stem + '.bat'
  else:
    return stem

ROOT = fs_utils.third_party_dir / 'depot_tools'
GN = ROOT / script_name('gn')
UPDATE_DEPOT_TOOLS = ROOT / script_name('update_depot_tools')
PYTHON3_BIN_RELDIR = ROOT / 'python3_bin_reldir.txt'

build.ExpandEnv('PATH', str(ROOT), prepend=True)

# cipd and vpython default their caches to the home directory.
os.environ.setdefault('CIPD_CACHE_DIR', str(fs_utils.build_dir / 'cipd_cache'))
os.environ.setdefault('VPYTHON_VIRTUALENV_ROOT', str(fs_utils.build_dir / 'vpython_root'))

def hook_recipe(recipe):
  recipe.add_step(
    git.clone('https://chromium.googlesource.com/chromium/tools/depot_tools.git', ROOT, 'main'),
    outputs=[UPDATE_DEPOT_TOOLS],
    inputs=[],
    state=[ROOT],
    desc='Downloading depot_tools',
    shortcut='get depot_tools')
  
  recipe.add_step(
    partial(Popen, [UPDATE_DEPOT_TOOLS], cwd=ROOT, shell=True),
    outputs=[PYTHON3_BIN_RELDIR],
    inputs=[UPDATE_DEPOT_TOOLS],
    desc='Updating depot_tools',
    shortcut='update depot_tools')

  recipe.add_step(
    lambda: None,
    outputs=[GN],
    inputs=[PYTHON3_BIN_RELDIR],
    desc='GN needs python3_bin_reldir.txt',
    shortcut='phony gn')
