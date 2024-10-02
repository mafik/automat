#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

if __name__ == '__main__':
  import subprocess, shutil
  from pathlib import Path

  root = Path(__file__).parent.parent.resolve()
  build_path = root / 'build'
  run_path = root / 'run.py'
  automat_path = build_path / 'release_automat.exe'
  test_state_path = root / 'tests' / 'event_bench.json'
  state_path = build_path / 'automat_state.json'

  subprocess.run(['python', str(run_path), 'link release_automat.exe'])
  shutil.copy(test_state_path, state_path)

  p = subprocess.Popen([str(automat_path)])
  p.wait()

  print('Done!')

  # TODO: Analyze resulting `automat_state.json`
  #    - drop any odd events at the end
  #    - use the first event as the basis to find expected delivery times
  #    - calculate the mean squared error