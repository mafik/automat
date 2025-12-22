# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import sys
import build
import extension_helper
import src
import fs_utils
from functools import partial
import make

if sys.platform == 'linux':

    xml2cpp = build.PREFIX / 'bin' / 'sdbus-c++-xml2cpp'

    def hook_srcs(srcs: dict[str, src.File], recipe: make.Recipe):
        for xml in (fs_utils.src_dir / 'dbus').glob('*.xml'):
            # For each .xml file in the dbus directory, generate _proxy.hh & _adaptor.hh
            for variant in ('proxy', 'adaptor'):
              path = (fs_utils.generated_dir / (xml.stem + f'_{variant}.hh'))
              recipe.add_step(
                  partial(make.Popen, [xml2cpp, f'--{variant}={path}', xml]),
                  [path],
                  [xml2cpp],
                  desc=f'Generating {path}',
                  shortcut=path.name)
              recipe.generated.add(str(path))
              srcs[str(path)] = src.File(path)


    libsystemd = src.load_extension('libsystemd')
    hook = extension_helper.ExtensionHelper('sdbus-cpp', globals())
    hook.FetchFromGit('https://github.com/Kistler-Group/sdbus-cpp.git', 'v2.2.1')
    hook.ConfigureDependsOn(libsystemd.hook)
    hook.ConfigureOptions(BUILD_SHARED_LIBS='OFF',
                          SDBUSCPP_BUILD_DOCS='OFF',
                          SDBUSCPP_BUILD_CODEGEN='ON')
    hook.ConfigureWithCMake(build.PREFIX / 'lib64' / 'libsdbus-c++.a', xml2cpp)
    hook.AddLinkArgs('-l:libsdbus-c++.a', '-lsystemd')
    hook.InstallWhenIncluded(r'^sdbus-c[+][+]/.*')
