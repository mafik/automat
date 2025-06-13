# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import extension_helper
import build

hook = extension_helper.ExtensionHelper('fmt', globals())

hook.FetchFromGit('https://github.com/fmtlib/fmt.git', '11.2.0')
hook.ConfigureWithCMake(build.PREFIX / 'include' / 'fmt' / 'format.h')
hook.ConfigureOptions(**{
    'FMT_DOC': 'OFF',
    'FMT_TEST': 'OFF',
    'FMT_FUZZ': 'OFF',
    'FMT_CUDA_TEST': 'OFF',
    'FMT_OS': 'ON',
    'FMT_MODULE': 'OFF',
    'FMT_SYSTEM_HEADERS': 'OFF'
})
hook.InstallWhenIncluded(r'fmt/.*\.h')
hook.AddCompileArgs('-I' + str(build.PREFIX / 'include'))
hook.AddLinkArgs('-lfmt' + build.debug_suffix)
