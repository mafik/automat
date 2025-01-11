'''Utilities for working with git repositories.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

from functools import partial
import make

def clone(url, out_directory, tag):
  '''Return an asynchronous 'git clone' wrapper.'''
  return partial(make.Popen, [
    'git',
    '-c', 'advice.detachedHead=false',
    '-c', 'core.autocrlf=false',
    'clone',
    '--depth', '1',
    '--branch', tag,
    url, out_directory])
