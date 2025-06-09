# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT
'''Rules for downloading depot_tools'''
import fs_utils
import git
import build

ROOT = fs_utils.third_party_dir / 'depot_tools'

build.ExpandEnv('PATH', str(ROOT), prepend=True)

def hook_recipe(recipe):
  recipe.add_step(
    git.clone('https://chromium.googlesource.com/chromium/tools/depot_tools.git', ROOT, 'main'),
    outputs=[ROOT],
    inputs=[],
    desc='Downloading depot_tools',
    shortcut='get depot_tools')
