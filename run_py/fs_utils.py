'''Utilities for operating on filesystem.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

from pathlib import Path

project_root = Path(__file__).resolve().parents[1]
project_name = Path(project_root).name.lower()
build_dir = project_root / 'build'
src_dir = project_root / 'src'
generated_dir = project_root / 'build' / 'generated'
third_party_dir = project_root / 'third_party'
