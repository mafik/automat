# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import build
import extension_helper

hook = extension_helper.ExtensionHelper('Leptonica', globals())

def leptonica_output(build_type : build.BuildType):
  if sys.platform == 'linux':
    return build_type.PREFIX() / 'lib' / 'libleptonica.a'
  elif sys.platform == 'win32':
    return build_type.PREFIX() / 'lib' / 'leptonica.lib'

hook.FetchFromURL('https://github.com/DanBloomberg/leptonica/releases/download/1.85.0/leptonica-1.85.0.tar.gz')
hook.ConfigureOptions(SW_BUILD='OFF',
                      ENABLE_ZLIB='OFF',
                      ENABLE_PNG='OFF',
                      ENABLE_GIF='OFF',
                      ENABLE_JPEG='OFF',
                      ENABLE_TIFF='OFF',
                      ENABLE_WEBP='OFF',
                      ENABLE_OPENJPEG='OFF',
                      CMAKE_C_FLAGS='-DNO_CONSOLE_IO=1')
hook.ConfigureWithCMake(src_dir=hook.checkout_dir, output=leptonica_output)
hook.AddLinkArg('-lleptonica')
if sys.platform == 'win32':
  hook.AddLinkArg(lambda t: ['-lmsvcrt' + ('d' if t == build.debug else '')])
hook.InstallWhenIncluded(r'^leptonica/.*\.h$')
