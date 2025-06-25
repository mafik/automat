# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

from extension_helper import ExtensionHelper
import build
from pathlib import Path

tracy = ExtensionHelper('tracy', globals())
tracy.FetchFromGit('https://github.com/wolfpld/tracy.git', 'v0.12.1')

# Warning: when building with CMake, tracy picks a wrong MSVC runtime library (dynamic instead of static)

# When building with Meson, tracy puts the library into "libtracy.a" instead of "tracy.lib"
meson_output = build.PREFIX / 'lib64' / 'libtracy.a'
desired_output = meson_output.with_name(build.libname('tracy'))
tracy.ConfigureWithMeson(meson_output)
tracy.ConfigureOptions(on_demand='true')

if meson_output.name != desired_output.name:
  def AliasTracyLib(token):
    if not desired_output.exists():
      desired_output.symlink_to(meson_output)
    token.touch()
  tracy.PostInstallStep(AliasTracyLib)

tracy.InstallWhenIncluded(r'tracy/Tracy\.hpp')
tracy.AddCompileArgs('-I', build.PREFIX / 'include' / 'tracy')
tracy.AddCompileArgs('-DTRACY_ENABLE', '-DTRACY_ON_DEMAND')
tracy.AddLinkArg('-ltracy')

if build.release:
  tracy.ConfigureOptions(only_localhost='true', no_broadcast='true')
  tracy.AddCompileArgs('-DTRACY_ONLY_LOCALHOST', '-DTRACY_NO_BROADCAST')
