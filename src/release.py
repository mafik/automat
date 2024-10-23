# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
from functools import partial
from make import Popen
import fs_utils
import build
import git
import os

RELEASE_HASH_FILE = fs_utils.build_dir / 'release_hash'
MAIN_HASH_FILE = fs_utils.build_dir / 'main_hash'

def hook_recipe(recipe):
  recipe.add_step(
    partial(Popen, ['git', 'rev-parse', '--verify', 'release'], stdout=RELEASE_HASH_FILE),
    outputs=[RELEASE_HASH_FILE],
    inputs=[],
    desc = 'Checking release hash',
    shortcut='check release hash',
    cleanup=lambda: os.unlink(RELEASE_HASH_FILE))
  
  recipe.add_step(
    partial(Popen, ['git', 'rev-parse', '--verify', 'main'], stdout=MAIN_HASH_FILE),
    outputs=[MAIN_HASH_FILE],
    inputs=[],
    desc = 'Checking main hash',
    shortcut='check main hash',
    cleanup=lambda: os.unlink(MAIN_HASH_FILE))
