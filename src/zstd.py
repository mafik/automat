# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import extension_helper
import build

hook = extension_helper.ExtensionHelper('zstd', globals())

hook.FetchFromGit('https://github.com/facebook/zstd.git', 'v1.5.7')
hook.SetSrcDir(hook.checkout_dir / 'build' / 'cmake')
hook.ConfigureWithCMake(build.PREFIX / 'include' / 'zstd.h')
hook.ConfigureOptions(**{
    'ZSTD_BUILD_PROGRAMS': 'OFF',
    'ZSTD_BUILD_SHARED': 'OFF',
    'ZSTD_BUILD_STATIC': 'ON',
    'ZSTD_BUILD_TESTS': 'OFF',
})

if build.platform == 'win32':
  hook.AddLinkArg('-lzstd_static')
else:
  hook.AddLinkArg('-lzstd')
