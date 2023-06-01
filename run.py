#!/usr/bin/env python3

'''Run Automat.'''

import subprocess
from python_modules import build_graph
from python_modules import debian_deps
from python_modules import args
import importlib
from sys import platform

debian_deps.check_and_install()

if __name__ == '__main__':
    if args.fresh:
        print('Cleaning old build results:')
        build_graph.recipe.clean()

    active_recipe = None

    while True:
        recipe = build_graph.recipe
        recipe.set_target(args.target)
        recipe.steps[-1].extra_args = args.extra_args

        if platform == 'linux':
            events = 'CLOSE_WRITE'
        elif platform == 'win32':
            events = 'create,modify,delete,move'
        else:
            raise Exception(f'Unknown platfrorm: "{platform}". Expected either "linux" or "win32". Automat is not supported on this platform yet!')

        # TODO: include inotify-win in the build scripts for Windows
        watcher = subprocess.Popen(['inotifywait', '-qe', events, 'src/'], stdout=subprocess.DEVNULL)

        if recipe.execute(watcher):
            if active_recipe:
                active_recipe.interrupt()
            active_recipe = recipe
        if not args.live:
            watcher.kill()
            break
        try:
            print('Watching src/ for changes...')
            watcher.wait()
        except KeyboardInterrupt:
            watcher.kill()
            break
        importlib.reload(build_graph)
