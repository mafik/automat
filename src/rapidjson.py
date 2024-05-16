from functools import partial
from subprocess import Popen
import fs_utils
import build

RAPIDJSON_ROOT = fs_utils.build_dir / 'rapidjson'
RAPIDJSON_INCLUDE = RAPIDJSON_ROOT / 'include'

build.base.compile_args += ['-I', RAPIDJSON_INCLUDE]

def hook_recipe(recipe):
  recipe.add_step(
      partial(Popen, ['git', 'clone', '--depth', '1', 'https://github.com/Tencent/rapidjson.git', RAPIDJSON_ROOT]),
      outputs=[RAPIDJSON_ROOT / 'CMakeLists.txt', RAPIDJSON_INCLUDE],
      inputs=[],
      desc = 'Downloading RapidJSON',
      shortcut='get rapidjson')

def hook_plan(srcs, objs, bins, recipe):
  for obj in objs:
    if any(inc.startswith('rapidjson') for inc in obj.source.system_includes):
      obj.deps.add(RAPIDJSON_INCLUDE)
