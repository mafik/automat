#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

'''Run Automat.'''

import sys

# Tool output (e.g. compiler warnings quoting UTF-8 sources) may not fit the console
# code page on Windows; degrade to replacement characters instead of crashing.
for stream in (sys.stdout, sys.stderr):
    if hasattr(stream, 'reconfigure'):
        stream.reconfigure(errors='replace')

from args import args
from sys import platform, exit, executable

if platform == 'win32':
    import windows_deps
    windows_deps.check_and_install()

import subprocess

import debian_deps
debian_deps.check_and_install()

import build

recipe = build.recipe()

from dashboard import Dashboard
dashboard = Dashboard(recipe)
dashboard.start()
if dashboard.exception:
  print(f"Dashboard failed to start: {dashboard.exception}")
else:
  print(f"Dashboard running at: http://localhost:{dashboard.port}/")

if args.fresh:
    print('Cleaning old build results:')
    recipe.clean()

active_recipe = None

while True:
    extra_targets = []
    if args.compile_commands:
        extra_targets.append('compile_commands.json')
    recipe.set_target(args.target, *extra_targets)
    if args.live:
        watcher = subprocess.Popen(
            [executable, 'run_py/inotify.py', 'src/', 'assets/'], stdout=subprocess.DEVNULL)
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
