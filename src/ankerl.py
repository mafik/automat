# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

import extension_helper
import build

hook = extension_helper.ExtensionHelper('ankerl', globals())

hook.FetchFromGit('https://github.com/martinus/unordered_dense.git', 'v4.8.1')
hook.ConfigureWithCMake(build.PREFIX / 'include' / 'ankerl' / 'unordered_dense.h')
hook.InstallWhenIncluded(r'ankerl/.*\.h')
