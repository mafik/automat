# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import build
import extension_helper
import src

leptonica = src.load_extension('leptonica')
hook = extension_helper.ExtensionHelper('Tesseract', globals())

def tesseract_output(build_type : build.BuildType):
  if sys.platform == 'linux':
    return build_type.PREFIX() / 'lib64' / 'libtesseract.a'
  elif sys.platform == 'win32':
    return build_type.PREFIX() / 'lib' / ('tesseract55d.lib' if build_type == build.debug else 'tesseract55.lib')

hook.FetchFromURL('https://github.com/tesseract-ocr/tesseract/archive/refs/tags/5.5.0.tar.gz', fetch_filename='tesseract-5.5.0.tar.gz')
hook.ConfigureDependsOn(leptonica.leptonica_output)
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
hook.ConfigureWithCMake(src_dir=hook.checkout_dir, output=tesseract_output)
if sys.platform == 'linux':
  hook.AddLinkArg('-ltesseract')
elif sys.platform == 'win32':
  hook.AddLinkArg(lambda t: ['-ltesseract55' + ('d' if t == build.debug else '')])
hook.InstallWhenIncluded(r'^tesseract/.*\.h$')
