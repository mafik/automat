# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
import build
from extension_helper import ExtensionHelper
from sys import platform
import subprocess

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
skia = ExtensionHelper('Skia', globals())
skia.FetchFromGit('https://skia.googlesource.com/skia.git', 'chrome/m136')
skia.ConfigureWithGN(skia.build_dir / build.libname('skia'))
skia.ninja_target = 'all'
skia.ConfigureOptions(
  skia_use_vulkan='true',
  skia_use_vma='true',
  skia_use_system_expat='false',
  skia_use_system_icu='false',
  skia_use_system_libjpeg_turbo='false',
  skia_use_system_libpng='false',
  skia_use_system_libwebp='false',
  skia_use_system_zlib='false',
  skia_use_system_harfbuzz='false',
  skia_enable_ganesh='false',
  skia_enable_graphite='true')

if build.fast:
  skia.ConfigureOptions(is_debug='false', is_official_build='true')
  build.compile_args += ['-DSK_RELEASE']
elif build.debug:
  skia.ConfigureOptions(is_debug='true', extra_cflags_cc='["-frtti"]')
  build.compile_args += ['-DSK_DEBUG']
elif build.release:
  skia.ConfigureOptions(is_debug='false', is_official_build='true')
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
  skia.ConfigureOptions(clang_win='"C:\\Program Files\\LLVM"', clang_win_version='20')
  if build.debug:
    skia.ConfigureOptions(extra_cflags='["/MTd"]')
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
  skia.ConfigureOptions(skia_use_system_freetype2='false')

build.compile_args += ['-I', skia.src_dir]
build.compile_args += ['-DSK_GRAPHITE']
build.compile_args += ['-DSK_VULKAN']
build.compile_args += ['-DSK_USE_VMA']
build.compile_args += ['-DSK_SHAPER_HARFBUZZ_AVAILABLE']

skia.InstallWhenIncluded(r'(include|src)/.*Sk.*\.h')
skia.AddLinkArgs('-L', skia.build_dir)

skia.PatchSources('''\
--- include/gpu/graphite/PrecompileContext.h
+++ include/gpu/graphite/PrecompileContext.h
@@ -13,6 +13,7 @@

 #include <chrono>
 #include <memory>
+#include <string>

 class SkData;

''')
def skia_git_sync_with_deps(marker):
  subprocess.run(['python', 'tools/git-sync-deps'], cwd=skia.src_dir, check=True)
  marker.touch()
skia.PatchSources(skia_git_sync_with_deps)
