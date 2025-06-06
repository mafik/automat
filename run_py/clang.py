# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
import subprocess
import shutil
from glob import glob
import re

def natural_sort(l): 
    convert = lambda text: int(text) if text.isdigit() else text.lower()
    alphanum_key = lambda key: [convert(c) for c in re.split('([0-9]+)', key)]
    return sorted(l, key=alphanum_key)

if shutil.which('clang'):
    executable = 'clang'
else:
    clang_binaries = glob('/usr/lib/llvm/*/bin/clang')
    if clang_binaries:
        clang_binaries = natural_sort(clang_binaries)
        executable = clang_binaries[-1]
    else:
        raise FileNotFoundError('Couldn\'t find `clang` program. Searched $PATH and `/usr/lib/llvm/*/bin/`.')

def query_default_defines() -> set[str]:
    result = subprocess.run([executable, '-dM', '-E', '-'],
                            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE)
    return set([line.split()[1] for line in result.stdout.decode().splitlines()])


default_defines = query_default_defines()
