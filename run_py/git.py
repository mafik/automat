'''Utilities for working with git repositories.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import os
import shutil
import stat
from pathlib import Path

import make

def clone(url, out_directory, tag):
  def runner():
    if Path(out_directory).exists():
      # Note: git marks some files as read-only, so we need to make them writable before we can delete them.
      shutil.rmtree(out_directory, onexc=lambda f, p, _: (os.chmod(p, stat.S_IWRITE), f(p)))
    return make.Popen([
      'git',
      '-c', 'advice.detachedHead=false',
      '-c', 'core.autocrlf=false',
      'clone',
      '--depth', '1',
      '--branch', tag,
      url, out_directory])

  return runner
