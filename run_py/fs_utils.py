'''Utilities for operating on filesystem.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import os
import shutil
import stat
import time
from pathlib import Path
from sys import platform

project_root = Path(__file__).resolve().parents[1]
project_name = Path(project_root).name.lower()
build_dir = project_root / 'build'
src_dir = project_root / 'src'
generated_dir = project_root / 'build' / 'generated'
third_party_dir = project_root / 'third_party'
run_py_dir = project_root / 'run_py'

binary_extension = '.exe' if platform == 'win32' else ''


def wipe(path):
  '''Removes a file or directory tree, if it exists.'''
  path = Path(path)
  if path.is_dir() and not path.is_symlink():
    # Note: git marks some files as read-only, so we need to make them writable before we can delete them.
    shutil.rmtree(path, onexc=lambda f, p, _: (os.chmod(p, stat.S_IWRITE), f(p)))
  else:
    path.unlink(missing_ok=True)


def move_aside(path):
  for stale in path.parent.glob(f'{path.name}.*.old'):
    try:
      stale.unlink()
    except PermissionError:
      pass
  if path.exists():
    aside = path.with_name(f'{path.name}.{time.time_ns()}.old')
    path.rename(aside)
    return aside
