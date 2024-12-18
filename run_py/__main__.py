#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

'''Run Automat.'''

import build
import subprocess
import debian_deps
from args import args
from sys import platform, exit

debian_deps.check_and_install()

recipe = build.recipe()

def print_step(step):
    print(' Step', step.shortcut)
    print('  Inputs:')
    for inp in sorted(str(x) for x in step.inputs):
        print('    ', inp)
    print('  Outputs: ', step.outputs)

if args.verbose:
    print('Build graph')
    for step in recipe.steps:
        print_step(step)

if args.fresh:
    print('Cleaning old build results:')
    recipe.clean()

active_recipe = None

while True:
    recipe.set_target(args.target)
    if args.live:
        watcher = subprocess.Popen(
            ['python', 'run_py/inotify.py', 'src/', 'assets/'], stdout=subprocess.DEVNULL)
    else:
        watcher = None

    try:
        ok = recipe.execute(watcher)
    except KeyboardInterrupt:
        exit(2)
    if ok:
        if active_recipe:
            active_recipe.interrupt()
        active_recipe = recipe
    if watcher:
        try:
            print('Watching src/ for changes...')
            watcher.wait()
        except KeyboardInterrupt:
            watcher.kill()
            break
    else:
        break
    # Reload the recipe because dependencies may have changed
    recipe = build.recipe()
if not ok:
    exit(1)
