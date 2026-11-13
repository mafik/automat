# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

import build
import extension_helper

debug_crt = build.platform == 'win32' and build.debug

if not debug_crt:
    def hook_plan(srcs, objs, bins, recipe):
        for binary in bins:
            binary.objects.append(build.ObjectFile(object_path))

hook = extension_helper.ExtensionHelper('mimalloc', globals())
hook.FetchFromGit('https://github.com/microsoft/mimalloc.git', 'v3.6.1')

if debug_crt:
    hook.SkipConfigure()
else:
    object_name = 'static.c.obj' if build.platform == 'win32' else 'static.c.o'
    object_path = hook.build_dir / 'CMakeFiles' / 'mimalloc-obj.dir' / 'src' / object_name
    hook.ConfigureWithCMake(object_path)
    hook.ninja_target = 'mimalloc-obj'
