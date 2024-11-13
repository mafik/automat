#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

if __name__ == '__main__':
  import subprocess, shutil
  from pathlib import Path
  import argparse

  parser = argparse.ArgumentParser(description='Run the event bench')
  parser.add_argument('--skip-run', action='store_true', help='Skip running the automat')
  parser.add_argument('--skip-plot', action='store_true', help='Skip plotting the results')
  parser.add_argument('--skip-record', action='store_true', help='Skip recording the results')

  args = parser.parse_args()

  root = Path(__file__).parent.parent.resolve()
  build_path = root / 'build'
  state_path = build_path / 'automat_state.json'

  if not args.skip_run:
    print('DO NOT take away the focus from the Automat window!')
    run_path = root / 'run.py'
    automat_path = build_path / 'release_automat'
    test_state_path = root / 'tests' / 'event_bench.json'

    subprocess.run(['python', str(run_path), 'link release_automat'])
    shutil.copy(test_state_path, state_path)

    p = subprocess.Popen([str(automat_path)])
    p.wait()

    print('Done!')

  import json

  state = json.load(open(state_path))

  def ms(t):
    return f'{t * 1000:.1f}ms'

  for loc in state['root']['locations']:
    if loc['type'] != 'Timeline':
      continue
    timestamps = None
    for track in loc['value']['tracks']:
      if track['name'] == 'Space':
        timestamps = track['timestamps']
        break
    if timestamps is None:
      continue
    if len(timestamps) < 20:
      # This is used to skip the original Timeline (the one with 10 equally spaced timestomps)
      # TODO: use UUID instead, once implemented
      continue

    errors = []
    start = 1
    expected_timestamps = [start + i * 0.1 for i in range(len(timestamps))]
    
    for i in range(len(timestamps)):
      error = timestamps[i] - expected_timestamps[i]
      errors.append(error)

    # Calculate jitter between events in the same series
    import math
    i = 1
    jitter = []
    while i < len(timestamps):
      curr_series = math.floor(i / 10)
      prev_series = math.floor((i - 1) / 10)
      if curr_series == prev_series:
        curr = timestamps[i]
        prev = timestamps[i - 1]
        delta = curr - prev
        jitter.append(delta - 0.1)
      i += 1

    max_jitter = max(abs(j) for j in jitter)

    # Calculate drift
    import numpy as np
    a, b = np.polyfit(expected_timestamps, timestamps, 1)
    drift = a - 1

    if args.skip_plot:
      print(f'Max jitter: {ms(max_jitter)}   Drift: {ms(drift)} per second')
    else:
      import matplotlib.pyplot as plt

      plt.title(f'Max jitter: {ms(max_jitter)}   Drift: {ms(drift)} per second')
      plt.errorbar(expected_timestamps, expected_timestamps, yerr=errors, uplims=[err < 0 for err in errors], lolims=[err > 0 for err in errors])
      plt.xlabel('Time')

      plt.show()

    if not args.skip_record:
      save = input('Record the results? [y/n] DID YOU TOUCH THE MOUSE DURING THE TEST? BECAUSE YOU SHOULDN\'T! ')
      if save == 'y':
        result_path = Path(__file__).parent / 'event_bench_log.json'
        results = json.load(open(result_path)) if result_path.exists() else dict()
        import datetime
        today = str(datetime.date.today())
        if today not in results:
          results[today] = {
            'max_jitter': [],
            'drift': []
          }
        results[today]['max_jitter'].append(round(max_jitter, 4))
        results[today]['drift'].append(round(drift, 4))
        json.dump(results, open(result_path, 'w'), indent=2)
      else:
        print('Results not saved')
