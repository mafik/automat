#! /usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

# This file needs a .py suffix in order to run under Windows.

import sys, os
from subprocess import run
from pathlib import Path

base_dir = Path(__file__).parent
exit_code = 0
try:
  completed_process = run(['python', str(base_dir / 'run_py')] + sys.argv[1:], cwd=base_dir)
  exit_code = completed_process.returncode
except KeyboardInterrupt:
  pass
except Exception as e:
  print(e)

# On Windows, if launched with double-click, keep
# the window open until the user presses ENTER.
if os.name == 'nt' and 'PROMPT' not in os.environ:
  input('Press ENTER to exit...')

sys.exit(exit_code)
