# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
import subprocess
import shutil
import os
from glob import glob
import re

# Required clang-20 features:
# - `#embed` into `char[]`
# - --warning-suppression-mappings
MIN_MAJOR = 20


def natural_sort(l):
    convert = lambda text: int(text) if text.isdigit() else text.lower()
    alphanum_key = lambda key: [convert(c) for c in re.split('([0-9]+)', key)]
    return sorted(l, key=alphanum_key)


def query_default_defines(clang_exe) -> dict[str, str]:
    '''Maps each predefined macro name to its value ('' when it has none).'''
    result = subprocess.run([clang_exe, '-dM', '-E', '-'],
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL)
    defines = {}
    for line in result.stdout.decode().splitlines():
        # Lines look like: #define NAME VALUE   (VALUE may be absent)
        parts = line.split(maxsplit=2)
        if len(parts) >= 2:
            defines[parts[1]] = parts[2] if len(parts) == 3 else ''
    return defines


def candidate_compilers():
    '''Yield clang C-compiler paths to consider, most-preferred first.'''

    # Whatever a plain `clang` resolves to comes first (may already be new enough).
    which = shutil.which('clang')
    yield which

    # Then explicitly versioned binaries (apt.llvm.org / Debian), newest first.
    versioned = [p for p in glob('/usr/bin/clang-*') +
                 glob('/usr/lib/llvm-*/bin/clang') +
                 glob('/usr/lib/llvm/*/bin/clang')
                 if re.search(r'/clang(-[0-9]+)?$', p)]
    for p in reversed(natural_sort(versioned)):
        if p != which:
            yield p


def find_compiler():
    '''Pick the first clang >= MIN_MAJOR. Returns (cc, cxx, defines); raises if none.'''
    checked = []
    for cc in candidate_compilers():
        head, base = os.path.split(cc)
        cxx = os.path.join(head, base.replace('clang', 'clang++', 1))
        defines = query_default_defines(cxx)
        major = int(defines.get('__clang_major__', '0') or '0')
        checked.append(f'{cc} (clang {major or "?"})')
        if major >= MIN_MAJOR:
            return cc, cxx, defines
    raise FileNotFoundError(
        f'Automat requires clang >= {MIN_MAJOR}, but none was found.\n'
        f'  Checked: {", ".join(checked) or "no clang on PATH"}\n'
        f'  Install clang-{MIN_MAJOR}+ (e.g. from https://apt.llvm.org/) and ensure it is on PATH.')


executable_c, executable, default_defines = find_compiler()
