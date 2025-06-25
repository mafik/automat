# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT
'''Rules for downloading the Meson build system'''

# TODO: move this to src/ and remove the empty hook_recipe function

import shutil
from pathlib import Path

BIN_STR = shutil.which('meson')

if BIN_STR:
  BIN = Path(BIN_STR)
  ARGS = [BIN]
  def hook_recipe(recipe):
    pass
else:
  import fs_utils, git

  DIR = fs_utils.third_party_dir / 'meson'
  BIN = DIR / 'meson.py'
  ARGS = ['python', str(BIN)]

  def hook_recipe(recipe):
    recipe.add_step(
        git.clone('https://github.com/mesonbuild/meson.git', DIR, '1.8.2'),
        outputs=[BIN],
        inputs=[],
        desc='Downloading Meson',
        shortcut='download meson')
