# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

import build
import extension_helper
import src

zlib = src.load_extension('zlib')

hook = extension_helper.ExtensionHelper('FFmpeg', globals())

hook.FetchFromGit('https://github.com/FFmpeg/FFmpeg.git', 'n7.1.5')
hook.ConfigureWithAutotools(
  build.PREFIX / 'lib64' / 'libavformat.a',
  build.PREFIX / 'lib64' / 'libavcodec.a',
  build.PREFIX / 'lib64' / 'libswscale.a',
  build.PREFIX / 'lib64' / 'libswresample.a',
  build.PREFIX / 'lib64' / 'libavutil.a')
hook.DependsOn(zlib.hook)

# LGPL build - never pass enable-gpl / enable-nonfree here.
hook.ConfigureOptions(**{
  'cc': build.compiler_c,  # FFmpeg's configure ignores $CC
  'enable-pic': '',
  'disable-autodetect': '',  # no system .so deps (xlib, vaapi, lzma, ...)
  'enable-zlib': '',
  'extra-cflags': f'-I{build.PREFIX / "include"}',
  'extra-ldflags': f'-L{build.PREFIX / "lib"} -L{build.PREFIX / "lib64"}',
  'disable-programs': '',
  'disable-doc': '',
  'disable-avdevice': '',
  'disable-avfilter': '',
  'disable-network': '',
  'disable-iconv': '',  # probed from libc even under disable-autodetect
  'disable-x86asm': '',  # build hosts have no nasm/yasm
})

hook.InstallWhenIncluded(r'lib(avformat|avcodec|avutil|swscale|swresample)/')
hook.AddLinkArgs('-lavformat', '-lavcodec', '-lswscale', '-lswresample', '-lavutil',
                 '-lm', '-lpthread')
