# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

import extension_helper
import build

hook = extension_helper.ExtensionHelper('pcre2', globals())

hook.FetchFromURL('https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.49/pcre2-10.49.tar.gz')
hook.ConfigureWithCMake(build.PREFIX / 'lib64' / 'libpcre2-8.a')
hook.ConfigureOptions(**{
  'PCRE2_BUILD_PCRE2GREP': 'OFF',
  'PCRE2_BUILD_TESTS': 'OFF',
  'PCRE2_SUPPORT_LIBBZ2': 'OFF',
  'PCRE2_SUPPORT_LIBZ': 'OFF',
  'PCRE2_SUPPORT_LIBREADLINE': 'OFF',
  'PCRE2_SUPPORT_JIT': 'ON',
})
