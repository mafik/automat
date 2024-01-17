#!/usr/bin/env python3

'''Run Automat.'''

import build
import subprocess
import debian_deps
from args import args
from sys import platform, exit

recipe = build.recipe()

if args.verbose:
    print('Build graph')
    for step in recipe.steps:
        print(' Step', step.shortcut)
        print('  Inputs:')
        for inp in sorted(str(x) for x in step.inputs):
            print('    ', inp)
        print('  Outputs: ', step.outputs)

if __name__ == '__main__':
    debian_deps.check_and_install()
    
    if args.fresh:
        print('Cleaning old build results:')
        recipe.clean()

    active_recipe = None

    while True:
        recipe.set_target(args.target)
        if platform == 'linux':
            events = 'CLOSE_WRITE'
        elif platform == 'win32':
            events = 'create,modify,delete,move'
        else:
            raise Exception(
                f'Unknown platfrorm: "{platform}". Expected either "linux" or "win32". Automat is not supported on this platform yet!')

        # TODO: include inotify-win in the build scripts for Windows
        watcher = subprocess.Popen(
            ['inotifywait', '-qe', events, 'src/'], stdout=subprocess.DEVNULL)

        ok = recipe.execute(watcher)
        if ok:
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
        # Reload the recipe because dependencies may have changed
        recipe = build.recipe()
    if not ok:
        exit(1)
