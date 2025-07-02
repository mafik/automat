# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT
import build
import extension_helper

libunwind = extension_helper.ExtensionHelper('libunwind', globals())
libunwind.FetchFromURL(
  'https://github.com/libunwind/libunwind/releases/download/v1.8.2/libunwind-1.8.2.tar.gz')
libunwind.ConfigureWithAutotools(build.PREFIX / 'lib64' / 'libunwind.a')
libunwind.ConfigureOption('enable-minidebuginfo', 'no') # disable LZMA dependency
libunwind.InstallWhenIncluded(r'^libunwind\.h')
libunwind.AddLinkArgs('-lunwind')
