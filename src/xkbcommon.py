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
  hook.ConfigureWithMeson('{PREFIX}/lib64/libxkbcommon.a')
  # libxkbcommon mixes C & statically-built C++ dependencies.
  # This confuses Meson. Here we explicitly tell it to link in the C++ stdlib.
  hook.ConfigureEnvReplaces(LDFLAGS='-lstdc++')
  hook.ConfigureOption('enable-x11', 'true')
  hook.ConfigureOption('enable-tools', 'false')
  hook.ConfigureOption('enable-xkbregistry', 'false')
  hook.ConfigureDependsOn(xcb.libxcb)
  hook.AddLinkArgs('-l:libxkbcommon.a', '-l:libxkbcommon-x11.a', '-l:libxcb-xkb.a')
  hook.InstallWhenIncluded(r'^xkbcommon/.*\.h$')
