# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT
import build
from extension_helper import ExtensionHelper

hook = ExtensionHelper('cavalier_contours', globals())
hook.FetchFromGit('https://github.com/jbuckmccready/CavalierContours.git', 'master')
hook.SkipConfigure()
hook.InstallWhenIncluded(r'cavc/.+\.hpp')
hook.AddCompileArg('-I' + str(hook.checkout_dir / 'include'))
