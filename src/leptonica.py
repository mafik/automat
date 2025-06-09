# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import build
import extension_helper

hook = extension_helper.ExtensionHelper('Leptonica', globals())

leptonica_output = build.PREFIX / 'lib' / build.libname('leptonica')

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
hook.ConfigureWithCMake(leptonica_output)
hook.AddLinkArg('-lleptonica')
if sys.platform == 'win32':
  hook.AddLinkArg('-lmsvcrt' + ('d' if build.debug else ''))
hook.InstallWhenIncluded(r'^leptonica/.*\.h$')
