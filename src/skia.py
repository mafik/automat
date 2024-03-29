
from dataclasses import dataclass
from functools import partial
from pathlib import Path
from sys import platform
import fs_utils
import os
import build
import re
import make

DEPOT_TOOLS_ROOT = fs_utils.build_dir / 'depot_tools'
SKIA_ROOT = fs_utils.build_dir / 'skia'
GN = (SKIA_ROOT / 'bin' / 'gn').with_suffix(build.binary_extension).absolute()

default_gn_args = 'skia_use_vulkan=true skia_use_vma=true'

default_gn_args += ' skia_use_system_expat=false'
default_gn_args += ' skia_use_system_icu=false'
default_gn_args += ' skia_use_system_libjpeg_turbo=false'
default_gn_args += ' skia_use_system_libpng=false'
default_gn_args += ' skia_use_system_libwebp=false'
default_gn_args += ' skia_use_system_zlib=false'
default_gn_args += ' skia_use_system_harfbuzz=false'

@dataclass
class BuildVariant:
  build_type: str
  build_dir: Path
  gn_args: str

variants = {
  '' : BuildVariant('', SKIA_ROOT / 'out' / 'Default', 'is_official_build=true'),
  'debug' : BuildVariant('debug', SKIA_ROOT / 'out' / 'Debug', 'is_debug=true'),
  'release' : BuildVariant('release', SKIA_ROOT / 'out' / 'Release', 'is_official_build=true is_debug=false'),
}

if platform == 'win32':
  build.default_compile_args += ['-DNOMINMAX']
  # Prefer UTF-8 over UTF-16. This means no "UNICODE" define.
  # https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page
  # DO NOT ADD: build.default_compile_args += ('UNICODE')
  # <windows.h> has a side effect of defining ERROR macro.
  # Adding NOGDI prevents it from happening.
  build.default_compile_args += ['-DNOGDI']
  # Silence some MSCRT-specific deprecation warnings.
  build.default_compile_args += ['-D_CRT_SECURE_NO_WARNINGS']
  # No clue what it precisely does but many projects use it.
  build.default_compile_args += ['-DWIN32_LEAN_AND_MEAN']
  build.default_compile_args += ['-DVK_USE_PLATFORM_WIN32_KHR']
  # Set Windows version to Windows 10.
  build.default_compile_args += ['-D_WIN32_WINNT=0x0A00']
  build.default_compile_args += ['-DWINVER=0x0A00']
  default_gn_args += ' clang_win="C:\Program Files\LLVM"'
  default_gn_args += ' clang_win_version=17'
  variants['debug'].gn_args += ' extra_cflags=["/MTd"]'
  # This subtly affects the Skia ABI and leads to crashes when passing sk_sp across the library boundary.
  # For more interesting defines, check out:
  # https://github.com/google/skia/blob/main/include/config/SkUserConfig.h
  build.debug_compile_args += ['-DSK_TRIVIAL_ABI=[[clang::trivial_abi]]']
elif platform == 'linux':
  build.default_compile_args += ['-DVK_USE_PLATFORM_XCB_KHR']

build.default_compile_args += ['-I', SKIA_ROOT]
build.default_compile_args += ['-DSK_GANESH']
build.default_compile_args += ['-DSK_VULKAN']
build.default_compile_args += ['-DSK_USE_VMA']
build.default_compile_args += ['-DSK_SHAPER_HARFBUZZ_AVAILABLE']

build.debug_compile_args += ['-DSK_DEBUG']

libname = build.libname('skia')

def git_clone_depot_tools():
  return make.Popen(['git', 'clone', '--depth', '1', 'https://chromium.googlesource.com/chromium/tools/depot_tools.git', DEPOT_TOOLS_ROOT])

def git_clone_skia():
  return make.Popen(['git', 'clone', '--depth', '1', 'https://skia.googlesource.com/skia.git', SKIA_ROOT])

def skia_git_sync_with_deps():
  return make.Popen(['python', 'tools/git-sync-deps'], cwd=SKIA_ROOT)

def skia_gn_gen(variant: BuildVariant):
  args = [GN, 'gen', variant.build_dir.relative_to(SKIA_ROOT), '--args=' + variant.gn_args + ' ' + default_gn_args]
  return make.Popen(args, cwd=SKIA_ROOT)

def skia_compile(variant: BuildVariant):
  args = ['ninja', '-C', variant.build_dir]
  return make.Popen(args)

def hook_recipe(recipe):
  recipe.add_step(git_clone_depot_tools, outputs=[fs_utils.build_dir / 'depot_tools'], inputs=[], desc='Downloading depot_tools', shortcut='get depot_tools')
  os.environ['PATH'] = str(DEPOT_TOOLS_ROOT) + ':' + os.environ['PATH']

  recipe.add_step(git_clone_skia, outputs=[SKIA_ROOT], inputs=[], desc='Downloading Skia', shortcut='get skia')
  
  recipe.add_step(skia_git_sync_with_deps, outputs=[GN], inputs=[SKIA_ROOT], desc='Syncing Skia git deps', shortcut='skia git sync with deps')

  for v in variants.values():
    args_gn = v.build_dir / 'args.gn'
    recipe.add_step(partial(skia_gn_gen, v), outputs=[args_gn], inputs=[GN, __file__], desc='Generating Skia build files', shortcut=('skia gn gen ' + v.build_type).strip())

    recipe.add_step(partial(skia_compile, v), outputs=[v.build_dir / libname], inputs=[args_gn], desc='Compiling Skia', shortcut=('skia ' + v.build_type).strip())

# Libraries offered by Skia
skia_libs = set(['shshaper', 'skunicode', 'skia', 'skottie'])

# Binaries that should link to Skia
skia_bins = set()

def hook_plan(srcs, objs, bins, recipe):
  for obj in objs:
    if any(re.match(r'(include|src)/.*Sk.*\.h', inc) for inc in obj.source.system_includes):
      obj.deps.add(SKIA_ROOT)

  for bin in bins:
    needs_skia = False
    for obj in bin.objects:
      if skia_libs.intersection(obj.source.comment_libs):
        needs_skia = True
        skia_bins.add(bin)
    if needs_skia:
      v = variants[bin.build_type]
      bin.link_args.append('-L' + str(v.build_dir))
        

def hook_final(srcs, objs, bins, recipe):
  for step in recipe.steps:
    needs_skia = False
    build_type = ''
    for bin in skia_bins:
      if str(bin.path) in step.outputs:
        needs_skia = True
        build_type = bin.build_type
        break
    if needs_skia:
      v = variants[build_type]
      step.inputs.add(str(v.build_dir / libname))