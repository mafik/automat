#!/usr/bin/env python

import shutil, subprocess
from pathlib import Path

tests_path = Path(__file__).parent.resolve()
root_path = tests_path.parent
build_path = root_path / 'build'
run_path = root_path / 'run.py'

// TODO: turn this into build graph

shutil.copy(tests_path / 'perf_bench.json', build_path / 'automat_state.json')
subprocess.run(['python', str(run_path), 'link debug_automat'], check=True)
subprocess.run(['time', 'perf',  'record', '--call-graph', 'fp',  'build/debug_automat'])
subprocess.run('perf script | third_party/FlameGraph/stackcollapse-perf.pl | third_party/FlameGraph/flamegraph.pl --width 1890 > build/flamegraph.svg', shell=True, check=True)
