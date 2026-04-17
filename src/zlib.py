# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import extension_helper
import build

hook = extension_helper.ExtensionHelper('zlib', globals())

hook.FetchFromGit('https://github.com/madler/zlib.git', 'v1.3.1')
hook.ConfigureWithCMake(build.PREFIX / 'include' / 'zlib.h')
hook.ConfigureOption('BUILD_SHARED_LIBS', 'OFF')
hook.ConfigureOption('CMAKE_POSITION_INDEPENDENT_CODE', 'ON')
if build.platform == 'win32':
  hook.AddLinkArg('-lzlibstatic')
else:
  hook.AddLinkArg('-lz')
