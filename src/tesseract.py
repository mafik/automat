# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import build
import extension_helper
import src

leptonica = src.load_extension('leptonica')
hook = extension_helper.ExtensionHelper('Tesseract', globals())

if sys.platform == 'linux':
  tesseract_output = build.PREFIX / 'lib64' / 'libtesseract.a'
elif sys.platform == 'win32':
  tesseract_output = build.PREFIX / 'lib64' / ('tesseract55d.lib' if build.Debug else 'tesseract55.lib')

hook.FetchFromURL('https://github.com/tesseract-ocr/tesseract/archive/refs/tags/5.5.0.tar.gz', fetch_filename='tesseract-5.5.0.tar.gz')
hook.ConfigureDependsOn(leptonica.hook)

hook.ConfigureOptions(
  SW_BUILD='OFF',
  OPENMP_BUILD='OFF',
  GRAPHICS_DISABLED='ON',
  DISABLED_LEGACY_ENGINE='ON',
  ENABLE_LTO='OFF',
  FAST_FLOAT='ON',
  ENABLE_NATIVE='OFF',
  BUILD_TRAINING_TOOLS='OFF',
  BUILD_TESTS='OFF',
  USE_SYSTEM_ICU='ON',
  DISABLE_TIFF='ON',
  DISABLE_ARCHIVE='ON',
  DISABLE_CURL='ON',
  INSTALL_CONFIGS='OFF')
if sys.platform == 'win32':
  hook.ConfigureOptions(WIN32_MT_BUILD='ON')
  # Windows SDK 10.0.26100.0 from May 2025 introduces a dependency from wchar.h to intrin.h (provided by Clang).
  # Clang seems to define its intrinsics using vector types, which don't work in MS-compatibility mode.
  # See: https://clang.llvm.org/docs/MSVCCompatibility.html#simd-vector-types
  # To work around this issue, we disable MS compatibility mode. This creates another issue with isascii, which
  # is defined in ctype.h (but only in MS-compatibility mode). We work around this one by defining it ourselves.
  #
  # The following line can be removed once Clang fixes its intrin.h. After removing, test it by building Automat
  # on GitHub Actions worker!
  hook.ConfigureOptions(CMAKE_CXX_FLAGS='-fno-ms-compatibility -Disascii=__isascii')
hook.ConfigureWithCMake(tesseract_output)
if sys.platform == 'linux':
  hook.AddLinkArg('-ltesseract')
elif sys.platform == 'win32':
  hook.AddLinkArg('-ltesseract55' + ('d' if build.Debug else ''))
hook.InstallWhenIncluded(r'^tesseract/.*\.h$')
