'''Utilities for operating on filesystem.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

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