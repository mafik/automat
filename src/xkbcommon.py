# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import build
import extension_helper
import src

if sys.platform == 'linux':
  xcb = src.load_extension('xcb')
  hook = extension_helper.ExtensionHelper('xkbcommon', globals())
  hook.FetchFromURL('https://github.com/xkbcommon/libxkbcommon/archive/refs/tags/xkbcommon-1.8.1.tar.gz')
  hook.ConfigureWithMeson(build.PREFIX / 'lib64' / 'libxkbcommon.a')
  # libxkbcommon mixes C & statically-built C++ dependencies.
  # This confuses Meson. Here we explicitly tell it to link in the C++ stdlib.
  #
  # TODO: Make the PKG_CONFIG_LIBDIR default for all extensions so they don't link against host's libs.
  hook.ConfigureEnvReplaces(
    LDFLAGS='-lstdc++',
    PKG_CONFIG_LIBDIR=':'.join(str(build.PREFIX / x / 'pkgconfig') for x in ('lib64', 'lib', 'share')),
  )
  hook.ConfigureOption('enable-x11', 'true')
  hook.ConfigureOption('enable-tools', 'false')
  hook.ConfigureOption('enable-xkbregistry', 'false')
  hook.ConfigureOption('xkb-config-root', '/usr/share/X11/xkb')
  hook.ConfigureDependsOn(xcb.libxcb)
  hook.AddLinkArgs('-l:libxkbcommon.a', '-l:libxkbcommon-x11.a', '-l:libxcb-xkb.a')
  hook.InstallWhenIncluded(r'^xkbcommon/.*\.h$')
