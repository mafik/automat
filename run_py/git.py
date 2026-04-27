'''Utilities for working with git repositories.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import shutil
from pathlib import Path

import make

def clone(url, out_directory, tag):
  out_directory = Path(out_directory)
  git_opts = '-c advice.detachedHead=false -c core.autocrlf=false'

  def runner():
    if (out_directory / '.git').is_dir():
      return make.Popen([
        f'git {git_opts} -C {out_directory} fetch --force --depth 1 origin {tag} && '
        f'git {git_opts} -C {out_directory} reset --hard FETCH_HEAD'],
        shell=True)
    if out_directory.exists():
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
