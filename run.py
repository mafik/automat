#! /usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

# This file needs a .py suffix in order to run under Windows.

import sys
from subprocess import run
from pathlib import Path

base_dir = Path(__file__).parent
run(['python', base_dir / 'run_py'] + sys.argv[1:])
