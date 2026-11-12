# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import build
import extension_helper

if sys.platform == 'linux':
    hook = extension_helper.ExtensionHelper('libucontext', globals())
    hook.FetchFromGit('https://github.com/kaniini/libucontext.git', 'libucontext-1.5.2')
    hook.ConfigureOptions(build_posix='false')
    hook.ConfigureWithMeson(build.PREFIX / 'lib64' / 'libucontext.a',
                            install_tags='devel',
                            build_targets=('libucontext.a',))
    hook.AddLinkArgs('-l:libucontext.a')
