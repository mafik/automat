# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

import extension_helper
import build

hook = extension_helper.ExtensionHelper('libffi', globals())

hook.FetchFromURL('https://github.com/libffi/libffi/releases/download/v3.8.0/libffi-3.8.0.tar.gz')
hook.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libffi.a')
hook.ConfigureOptions(**{
  'disable-docs': '',
  'disable-multi-os-directory': '',
})
