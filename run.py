#!/usr/bin/env python3

'''Run Automaton.'''

import sys
sys.path.append('python')
import subprocess
import cc
import build_graph
import debian_deps
import args
import importlib

debian_deps.check_and_install()

if __name__ == '__main__':
    if args.fresh:
        print('Cleaning old build results:')
        build_graph.recipe.clean()

    active_recipe = None

    while True:
        recipe = build_graph.recipe
        recipe.set_target(args.target)

        watcher = subprocess.Popen(['inotifywait', '-qe', 'CLOSE_WRITE', 'src/'])
        if recipe.execute(watcher):
            if active_recipe:
                active_recipe.interrupt()
            active_recipe = recipe
        if not args.live:
            break
        try:
            print('Watching src/ for changes...')
            watcher.wait()
        except KeyboardInterrupt:
            watcher.kill()
            break
        importlib.reload(cc)
        importlib.reload(build_graph)
