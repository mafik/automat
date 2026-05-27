'''Utilities for working with git repositories.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import os
import shutil
import stat
import glob
from pathlib import Path
from functools import partial

# Safe (& idempotent) wrapper around git clone.
# Creates a tombstone before running the command.
# If a tombstone already exists, removes the git dir before cloning.
if __name__ == '__main__':
  import sys
  import subprocess

  tag, url, out_directory = sys.argv[1:] # layout defined by clone() below
  out_directory = Path(out_directory)

  # Tombstone indicates that something went wrong during last clone attempt.
  sentinel = out_directory.with_suffix('.cloning_sentinel')
  if sentinel.exists():
    # Something went wrong during last attempt!
    # Nuke the old checkout - will force a fresh clone.
    print(f'Tombstone found at {sentinel}, removing {out_directory} before cloning.')
    # Note: git marks some files as read-only, so we need to make them writable before we can delete them.
    shutil.rmtree(out_directory, onexc=lambda f, p, _: (os.chmod(p, stat.S_IWRITE), f(p)))
  else:
    # Create the tombstone before attempting any risky operations.
    sentinel.touch()

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
  
  sentinel.unlink()

else: # for importing as a module
  import make

  def clone(url, out_directory, tag):
    return partial(make.Popen, ['python', __file__, tag, url, out_directory])