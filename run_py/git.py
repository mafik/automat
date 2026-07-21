'''Utilities for working with git repositories.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import os
import glob
import sys
from pathlib import Path
from functools import partial


# Safe (& idempotent) wrapper around git clone.
if __name__ == '__main__':
  import subprocess

  tag, url, out_directory = sys.argv[1:] # layout defined by clone() below
  out_directory = Path(out_directory)

  if out_directory.exists():
    subprocess.run(['git',
      '-c', 'advice.detachedHead=false',
      '-c', 'core.autocrlf=false',
      'fetch', '--depth', '1', url, tag], cwd=out_directory, check=True)
    subprocess.run(['git', 'reset', '--hard', 'FETCH_HEAD'], cwd=out_directory, check=True)
    for marker in glob.glob('*.marker', root_dir = out_directory):
      os.unlink(out_directory / marker)
    os.utime(out_directory, None)  # bump mtime so make.py sees the update
  else:
    subprocess.run(['git',
      '-c', 'advice.detachedHead=false',
      '-c', 'core.autocrlf=false',
      'clone',
      '--depth', '1', '--branch', tag, url, str(out_directory)], check=True)

else: # for importing as a module
  import make

  def clone(url, out_directory, tag):
    return partial(make.Popen, [sys.executable, __file__, tag, url, out_directory])
