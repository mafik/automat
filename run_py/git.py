'''Utilities for working with git repositories.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import shutil
from pathlib import Path

import make

def clone(url, out_directory, tag):
  def runner():
    if Path(out_directory).exists():
      shutil.rmtree(out_directory)
    return make.Popen([
      'git',
      '-c', 'advice.detachedHead=false',
      '-c', 'core.autocrlf=false',
      'clone',
      '--depth', '1',
      '--branch', tag,
      url, out_directory])

  return runner
