# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
from dataclasses import dataclass
from functools import partial
from pathlib import Path
from sys import platform
import fs_utils
import os
import build
import re
import make
import ninja
import git

TAG = 'chrome/m130'
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
default_gn_args += ' skia_enable_ganesh=false'
default_gn_args += ' skia_enable_graphite=true'

@dataclass
class BuildVariant:
  build_type: build.BuildType
  gn_args: str

  def __init__(self, build_type: build.BuildType, gn_args: str):
    self.build_type = build_type
    self.gn_args = gn_args
    self.build_dir = SKIA_ROOT / 'out' / self.build_type.name

variants = {
  'Fast' : BuildVariant(build.fast, 'is_official_build=true'),
  'Debug' : BuildVariant(build.debug, 'is_debug=true extra_cflags_cc=["-frtti"]'),
  'Release' : BuildVariant(build.release, 'is_official_build=true is_debug=false'),
}

if platform == 'win32':
  build.base.compile_args += ['-DNOMINMAX']
  # Prefer UTF-8 over UTF-16. This means no "UNICODE" define.
  # https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page
  # DO NOT ADD: build.base.compile_args += ('UNICODE')
  # <windows.h> has a side effect of defining ERROR macro.
  # Adding NOGDI prevents it from happening.
  build.base.compile_args += ['-DNOGDI']
  # Silence some MSCRT-specific deprecation warnings.
  build.base.compile_args += ['-D_CRT_SECURE_NO_WARNINGS']
  # No clue what it precisely does but many projects use it.
  build.base.compile_args += ['-DWIN32_LEAN_AND_MEAN']
  build.base.compile_args += ['-DVK_USE_PLATFORM_WIN32_KHR']
  # Set Windows version to Windows 10.
  build.base.compile_args += ['-D_WIN32_WINNT=0x0A00']
  build.base.compile_args += ['-DWINVER=0x0A00']
  default_gn_args += ' clang_win="C:\\Program Files\\LLVM"'
  default_gn_args += ' clang_win_version=17'
  variants['Debug'].gn_args += ' extra_cflags=["/MTd"]'
  # This subtly affects the Skia ABI and leads to crashes when passing sk_sp across the library boundary.
  # For more interesting defines, check out:
  # https://github.com/google/skia/blob/main/include/config/SkUserConfig.h
  build.debug.compile_args += ['-DSK_TRIVIAL_ABI=[[clang::trivial_abi]]']
elif platform == 'linux':
  build.base.compile_args += ['-DVK_USE_PLATFORM_XCB_KHR']
  # Static linking to Vulkan on Linux seems to fail because the dynamically loaded libc / pthread is not properly initialized
  # by dlopen. It seems that glibc is closely coupled with ld.
  # Note that this problem might disappear when executed under debugger because they might be initialized when debugger ld runs.
  # Some discussion can be found here: https://www.openwall.com/lists/musl/2012/12/08/4
  # Most online discussions wrongly claim its nonsensical to call dlopen from a static binary because this loses the static linking benefits.
  # It is in fact useful, for example because it might allow Automat to bundle its dependencies but still use the system's vulkan drivers.
  # Anyway there is no clean, officially supported solution to dlopen from a static binary on Linux :(
  # A potential, slightly hacky workaround might be found here: https://github.com/pfalcon/foreign-dlopen/tree/master
  # For the time being we disable static linking on Linux and instead statically link everything except libm & libc (they seem to be coupled).
  build.base.compile_args = [x for x in build.base.compile_args if x != '-static']
  build.base.link_args = [x for x in build.base.link_args if x != '-static']
  build.base.link_args += ['-Wl,-Bstatic', '-static-libstdc++', '-lpthread', '-static-libgcc', '-ldl', '-Wl,-Bdynamic']
  default_gn_args += ' skia_use_system_freetype2=false'


build.base.compile_args += ['-I', SKIA_ROOT]
build.base.compile_args += ['-DSK_GRAPHITE']
build.base.compile_args += ['-DSK_VULKAN']
build.base.compile_args += ['-DSK_USE_VMA']
build.base.compile_args += ['-DSK_SHAPER_HARFBUZZ_AVAILABLE']

build.debug.compile_args += ['-DSK_DEBUG']

libname = build.libname('skia')

def skia_git_sync_with_deps():
  return make.Popen(['python', 'tools/git-sync-deps'], cwd=SKIA_ROOT)

def skia_gn_gen(variant: BuildVariant):
  args = [GN, 'gen', variant.build_dir.relative_to(SKIA_ROOT), '--args=' + variant.gn_args + ' ' + default_gn_args]
  return make.Popen(args, cwd=SKIA_ROOT)

def skia_compile(variant: BuildVariant):
  args = [ninja.BIN, '-C', variant.build_dir]
  return make.Popen(args)

def hook_recipe(recipe):
  recipe.add_step(
    git.clone('https://chromium.googlesource.com/chromium/tools/depot_tools.git', DEPOT_TOOLS_ROOT, 'main'),
    outputs=[fs_utils.build_dir / 'depot_tools'],
    inputs=[],
    desc='Downloading depot_tools',
    shortcut='get depot_tools')
  os.environ['PATH'] = str(DEPOT_TOOLS_ROOT) + ':' + os.environ['PATH']

  recipe.add_step(
    git.clone('https://skia.googlesource.com/skia.git', SKIA_ROOT, TAG),
    outputs=[SKIA_ROOT],
    inputs=[],
    desc='Downloading Skia',
    shortcut='get skia')
  
  recipe.add_step(skia_git_sync_with_deps, outputs=[GN], inputs=[SKIA_ROOT], desc='Syncing Skia git deps', shortcut='skia git sync with deps')

  for v in variants.values():
    args_gn = v.build_dir / 'args.gn'
    recipe.add_step(partial(skia_gn_gen, v), outputs=[args_gn], inputs=[GN, __file__], desc='Generating Skia build files', shortcut='skia gn gen' + v.build_type.rule_suffix())

    recipe.add_step(partial(skia_compile, v), outputs=[v.build_dir / libname], inputs=[ninja.BIN, args_gn], desc='Compiling Skia', shortcut='skia' + v.build_type.rule_suffix())

# Libraries offered by Skia
skia_libs = set(['shshaper', 'skunicode', 'skia', 'skottie', 'svg'])

# Binaries that should link to Skia
skia_bins = set()

def get_variant(build_type):
  if build_type.name in variants:
    return variants[build_type.name]
  return get_variant(build_type.base)

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
      v = get_variant(bin.build_type)
      bin.link_args.append('-L' + str(v.build_dir))
        

def hook_final(srcs, objs, bins, recipe):
  for step in recipe.steps:
    needs_skia = False
    build_type = None
    for bin in skia_bins:
      if str(bin.path) in step.outputs:
        needs_skia = True
        build_type = bin.build_type
        break
    if needs_skia:
      v = get_variant(build_type)
      step.inputs.add(str(v.build_dir / libname))
