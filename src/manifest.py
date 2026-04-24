# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT
from sys import platform
from functools import partial

import build
import fs_utils
import make

if platform == 'win32':
    MANIFEST_RC = fs_utils.project_root / 'src' / 'manifest.rc'
    MANIFEST_XML = fs_utils.project_root / 'src' / 'manifest.xml'

    def hook_plan(srcs, objs, bins, recipe):
        wanting_bins = [b for b in bins
                        if any(getattr(o.source, 'manifest', False) for o in b.objects)]
        if not wanting_bins:
            return

        manifest_res = build.BASE / 'manifest.res'
        recipe.add_step(
            partial(make.Popen, [
                'llvm-rc',
                '/FO', str(manifest_res),
                '/I', str(fs_utils.project_root / 'src'),
                str(MANIFEST_RC),
            ]),
            outputs=[manifest_res],
            inputs=[MANIFEST_RC, MANIFEST_XML],
            desc='Compiling manifest.rc',
            shortcut='manifest.res',
        )
        for binary in wanting_bins:
            binary.objects.append(build.ObjectFile(manifest_res))
