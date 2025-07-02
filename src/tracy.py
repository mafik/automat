# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

from extension_helper import ExtensionHelper
from sys import platform
import build

tracy = ExtensionHelper('tracy', globals())
tracy.FetchFromGit('https://github.com/wolfpld/tracy.git', 'v0.12.1')
tracy.SkipConfigure()

# Warning: Tracy likes to pick a wrong MSVC runtime library (dynamic instead of
# static or release instead of debug) when built with CMake or Meson.

tracy.InstallWhenIncluded(r'^(tracy/.*|TracyClient\.cpp)')
tracy.AddCompileArgs('-I', build.PREFIX / 'include' / 'tracy')
tracy.AddCompileArgs('-I', tracy.src_dir / 'public')
tracy.AddCompileArgs('-DTRACY_ENABLE', '-DTRACY_ON_DEMAND')


if platform == 'linux':
  tracy.AddCompileArgs('-DTRACY_LIBUNWIND_BACKTRACE')

if build.release:
  tracy.ConfigureOptions(only_localhost='true', no_broadcast='true')
  tracy.AddCompileArgs('-DTRACY_ONLY_LOCALHOST', '-DTRACY_NO_BROADCAST')
