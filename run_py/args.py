'''Module with all of the command line flags used by the build script.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

# TODO: allow different python modules to add their own args - instead of a centralized location. This should allow for better Hyperdeck / Automat reusability.

import __main__
import argparse
import sys
import build_variant

sys.argv[0] = 'run'

parser = argparse.ArgumentParser(
    description=__main__.__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--fresh', action='store_true')
parser.add_argument('--live', action='store_true')
parser.add_argument('--no-compile-commands', action='store_false', dest='compile_commands', help='Disable generation of compile_commands.json')
parser.add_argument('--verbose', action='store_true')
parser.add_argument('target')
parser.add_argument('--variant', choices=build_variant.index.keys(), default='fast')
parser.add_argument('-x', action='append',
                    help='argument passed to the target', dest='extra_args', default=[])
args = parser.parse_args()

build_variant.index[args.variant].set_as_current()
