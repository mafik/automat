# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
from dataclasses import dataclass
from functools import partial
from pathlib import Path
from sys import platform
import fs_utils
import os
import build
import build_variant
import re
import make
import ninja
import git

# TODO: milestone 137 includes a change that requires all Ganesh window surfaces to support VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT.
# https://source.chromium.org/chromium/_/skia/skia/+/6627deb65939ee886c774d290d91269c6968eaf9
# This feature is not supported by Windows Intel GPUs so we're sticking with milestone 136 for now (until the issue gets fixed).
# To see if Skia is fixed, configure it with:
# > bin\gn gen out/Static --args="skia_use_vulkan=true skia_enable_ganesh=true skia_enable_graphite=true skia_enable_tools=true"
# then build the viewer:
# > ninja -C out\Static viewer
# and run it (vulkan mode):
# > out\Static\viewer -b vk
# Finally, switch to the Graphite mode by typing '/' and changing the backend to 'graphite'.
# The viewer should not crash with:
# [graphite] ** ERROR ** validate_backend_texture failed: backendTex.info = Vulkan(viewFormat=BGRA8,flags=0x00000000,imageTiling=0,imageUsageFlags=0x00000017,sharingMode=0,aspectMask=1,bpp=4,sampleCount=1,mipmapped=0,protected=0); colorType = 6
TAG = 'chrome/m136'
DEPOT_TOOLS_ROOT = fs_utils.third_party_dir / 'depot_tools'
SKIA_ROOT = fs_utils.third_party_dir / 'Skia'
GN = (SKIA_ROOT / 'bin' / 'gn').with_suffix(fs_utils.binary_extension).absolute()

gn_args = 'skia_use_vulkan=true skia_use_vma=true'

gn_args += ' skia_use_system_expat=false'
gn_args += ' skia_use_system_icu=false'
gn_args += ' skia_use_system_libjpeg_turbo=false'
gn_args += ' skia_use_system_libpng=false'
gn_args += ' skia_use_system_libwebp=false'
gn_args += ' skia_use_system_zlib=false'
gn_args += ' skia_use_system_harfbuzz=false'
gn_args += ' skia_enable_ganesh=false'
gn_args += ' skia_enable_graphite=true'

build_dir = build.BASE / 'Skia'

if build.Fast:
  gn_args += ' is_debug=false is_official_build=true'
  build.compile_args += ['-DSK_RELEASE']
elif build.Debug:
  gn_args += ' is_debug=true extra_cflags_cc=["-frtti"]'
  build.compile_args += ['-DSK_DEBUG']
elif build.Release:
  gn_args += ' is_debug=false is_official_build=true'
  build.compile_args += ['-DSK_RELEASE']

if platform == 'win32':
  build.compile_args += ['-DNOMINMAX']
  # Prefer UTF-8 over UTF-16. This means no "UNICODE" define.
  # https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page
  # DO NOT ADD: build.compile_args += ('UNICODE')
  # <windows.h> has a side effect of defining ERROR macro.
  # Adding NOGDI prevents it from happening.
  build.compile_args += ['-DNOGDI']
  # Silence some MSCRT-specific deprecation warnings.
  build.compile_args += ['-D_CRT_SECURE_NO_WARNINGS']
  # No clue what it precisely does but many projects use it.
  build.compile_args += ['-DWIN32_LEAN_AND_MEAN']
  build.compile_args += ['-DVK_USE_PLATFORM_WIN32_KHR']
  # Set Windows version to Windows 10.
  build.compile_args += ['-D_WIN32_WINNT=0x0A00']
  build.compile_args += ['-DWINVER=0x0A00']
  gn_args += ' clang_win="C:\\Program Files\\LLVM"'
  gn_args += ' clang_win_version=20'
  if build.Debug:
    gn_args += ' extra_cflags=["/MTd"]'
    # This subtly affects the Skia ABI and leads to crashes when passing sk_sp across the library boundary.
    # For more interesting defines, check out:
    # https://github.com/google/skia/blob/main/include/config/SkUserConfig.h
    build.compile_args += ['-DSK_TRIVIAL_ABI=[[clang::trivial_abi]]']
elif platform == 'linux':
  build.compile_args += ['-DVK_USE_PLATFORM_XCB_KHR']
  # Static linking to Vulkan on Linux seems to fail because the dynamically loaded libc / pthread is not properly initialized
  # by dlopen. It seems that glibc is closely coupled with ld.
  # Note that this problem might disappear when executed under debugger because they might be initialized when debugger ld runs.
  # Some discussion can be found here: https://www.openwall.com/lists/musl/2012/12/08/4
  # Most online discussions wrongly claim its nonsensical to call dlopen from a static binary because this loses the static linking benefits.
  # It is in fact useful, for example because it might allow Automat to bundle its dependencies but still use the system's vulkan drivers.
  # Anyway there is no clean, officially supported solution to dlopen from a static binary on Linux :(
  # A potential, slightly hacky workaround might be found here: https://github.com/pfalcon/foreign-dlopen/tree/master
  # For the time being we disable static linking on Linux and instead statically link everything except libm & libc (they seem to be coupled).
  build.compile_args = [x for x in build.compile_args if x != '-static']
  build.link_args = [x for x in build.link_args if x != '-static']
  build.link_args += ['-static-libstdc++', '-static-libgcc', '-ldl']
  gn_args += ' skia_use_system_freetype2=false'

build.compile_args += ['-I', SKIA_ROOT]
build.compile_args += ['-DSK_GRAPHITE']
build.compile_args += ['-DSK_VULKAN']
build.compile_args += ['-DSK_USE_VMA']
build.compile_args += ['-DSK_SHAPER_HARFBUZZ_AVAILABLE']


libname = build.libname('skia')

def skia_git_sync_with_deps():
  return make.Popen(['python', 'tools/git-sync-deps'], cwd=SKIA_ROOT)

def skia_gn_gen():
  args = [GN, 'gen', build_dir, '--script-executable=python', '--args=' + gn_args]
  return make.Popen(args, cwd=SKIA_ROOT)

def skia_compile():
  args = [ninja.BIN, '-C', build_dir]
  return make.Popen(args)

def hook_recipe(recipe):
  recipe.add_step(
    git.clone('https://chromium.googlesource.com/chromium/tools/depot_tools.git', DEPOT_TOOLS_ROOT, 'main'),
    outputs=[DEPOT_TOOLS_ROOT],
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
  
  recipe.add_step(skia_git_sync_with_deps, outputs=[GN], inputs=[SKIA_ROOT, DEPOT_TOOLS_ROOT], desc='Syncing Skia git deps', shortcut='skia git sync with deps')

  args_gn = build_dir / 'args.gn'
  build_ninja = build_dir / 'build.ninja'
  recipe.add_step(skia_gn_gen, outputs=[args_gn, build_ninja], inputs=[GN, __file__], desc='Generating Skia build files', shortcut='skia gn gen')
  recipe.add_step(skia_compile, outputs=[build_dir / libname], inputs=[ninja.BIN, args_gn, build_ninja], desc='Compiling Skia', shortcut='skia')

# Libraries offered by Skia
skia_libs = set(['shshaper', 'skunicode', 'skia', 'skottie', 'svg'])

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
      bin.link_args.append('-L' + str(build_dir))
        

def hook_final(srcs, objs, bins, recipe):
  for step in recipe.steps:
    needs_skia = False
    for bin in skia_bins:
      if str(bin.path) in step.outputs:
        needs_skia = True
        break
    if needs_skia:
      step.inputs.add(str(build_dir / libname))
