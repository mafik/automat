# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import extension_helper
import build
import shutil

hook = extension_helper.ExtensionHelper('zlib', globals())

hook.FetchFromGit('https://github.com/madler/zlib.git', 'v1.3.1')

# FFmpeg's config.h defines HAVE_UNISTD_H to 0 on Windows, which #ifdef can't tell apart.
hook.PatchSources('''--- zconf.h.cmakein
+++ zconf.h.cmakein
@@ -435,7 +435,7 @@
    typedef unsigned long z_crc_t;
 #endif

-#ifdef HAVE_UNISTD_H    /* may be set to #if 1 by ./configure */
+#if defined(HAVE_UNISTD_H) && HAVE_UNISTD_H+0 != 0    /* may be set to #if 1 by ./configure */
 #  define Z_HAVE_UNISTD_H
 #endif

''')

hook.ConfigureWithCMake(build.PREFIX / 'include' / 'zlib.h')
hook.ConfigureOption('BUILD_SHARED_LIBS', 'OFF')
hook.ConfigureOption('CMAKE_POSITION_INDEPENDENT_CODE', 'ON')
if build.platform == 'win32':
  def InstallZAlias(marker):
    # FFmpeg's configure probes zlib with -lz, which lld-link resolves as z.lib.
    shutil.copy2(build.PREFIX / 'lib' / 'zlibstatic.lib', build.PREFIX / 'lib' / 'z.lib')
    marker.touch()
  hook.PostInstallStep(InstallZAlias)
  hook.AddLinkArg('-lzlibstatic')
else:
  def RemoveSharedZlib(marker):
    for so in (build.PREFIX / 'lib').glob('libz.so*'):
      so.unlink()
    marker.touch()
  hook.PostInstallStep(RemoveSharedZlib)
  hook.AddLinkArg('-l:libz.a')
