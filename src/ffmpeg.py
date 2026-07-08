# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

import build
import extension_helper
import src

zlib = src.load_extension('zlib')

hook = extension_helper.ExtensionHelper('FFmpeg', globals())

hook.FetchFromGit('https://github.com/FFmpeg/FFmpeg.git', 'n7.1.5')

# Windows' 32 KB command line limit truncates the ~700-object ar invocations;
# a response file carries the object list instead (GNU make >= 4.0, ar @file).
hook.PatchSources('''--- ffbuild/library.mak
+++ ffbuild/library.mak
@@ -35,5 +35,7 @@
 endif
 $(SUBDIR)$(LIBNAME): $(OBJS) $(STLIBOBJS)
 \t$(RM) $@
-\t$(AR) $(ARFLAGS) $(AR_O) $^
+\t$(file >$@.objs,$^)
+\t$(AR) $(ARFLAGS) $(AR_O) @$@.objs
+\t$(RM) $@.objs
 \t$(RANLIB) $@
''')

hook.ConfigureWithAutotools(
  build.PREFIX / 'lib64' / 'libavformat.a',
  build.PREFIX / 'lib64' / 'libavcodec.a',
  build.PREFIX / 'lib64' / 'libswscale.a',
  build.PREFIX / 'lib64' / 'libswresample.a',
  build.PREFIX / 'lib64' / 'libavutil.a')
hook.DependsOn(zlib.hook)

# LGPL build - never pass enable-gpl / enable-nonfree here.
hook.ConfigureOptions(**{
  # FFmpeg's configure ignores $CC; the bare name sidesteps spaces in the Windows clang path.
  'cc': 'clang' if build.platform == 'win32' else build.compiler_c,
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

if build.platform == 'win32':
  # FFmpeg's Makefiles need GNU make, which Windows lacks. ezwinports ships a
  # native build that runs its recipes through the sh found on PATH (Git's).
  gnu_make = extension_helper.ExtensionHelper('gnu_make', globals())
  gnu_make.FetchFromURL('https://downloads.sourceforge.net/project/ezwinports/make-4.4.1-without-guile-w32-bin.zip')
  gnu_make.SkipConfigure()
  hook.make_exe = str(gnu_make.checkout_dir / 'bin' / 'make.exe')
  hook.ConfigureDependsOn(gnu_make.checkout_dir)
  # Invoked as src/configure (through a junction), configure keeps SRC_PATH
  # relative; the /c/... paths its pwd would emit are unreadable to native make.
  hook.configure_src_link = True

  hook.ConfigureOptions(**{
    # git-bash's uname says MSYS_NT, which configure rejects; win64 (not mingw64)
    # because clang targets MSVC and its link.exe rejects mingw64's GNU ldflags.
    'target-os': 'win64',
    'ar': 'llvm-ar',
    'nm': 'llvm-nm',
  })
  hook.AddLinkArgs(*[str(build.PREFIX / 'lib64' / f'lib{name}.a')
                     for name in ('avformat', 'avcodec', 'swscale', 'swresample', 'avutil')],
                   '-lbcrypt')
else:
  hook.AddLinkArgs('-lavformat', '-lavcodec', '-lswscale', '-lswresample', '-lavutil',
                   '-lm', '-lpthread')
