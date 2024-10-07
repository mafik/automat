# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
from functools import partial
from subprocess import Popen
import fs_utils
import build
import git

RAPIDJSON_ROOT = fs_utils.build_dir / 'rapidjson'
RAPIDJSON_INCLUDE = RAPIDJSON_ROOT / 'include'

build.base.compile_args += ['-I', RAPIDJSON_INCLUDE]

def hook_recipe(recipe):
  recipe.add_step(
      git.clone('https://github.com/Tencent/rapidjson.git', RAPIDJSON_ROOT, 'master'),
      outputs=[RAPIDJSON_ROOT / 'CMakeLists.txt', RAPIDJSON_INCLUDE],
      inputs=[],
      desc = 'Downloading RapidJSON',
      shortcut='get rapidjson')

def hook_plan(srcs, objs, bins, recipe):
  for obj in objs:
    if any(inc.startswith('rapidjson') for inc in obj.source.system_includes):
      obj.deps.add(RAPIDJSON_INCLUDE)
