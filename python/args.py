'''Module with all of the command line flags used by the build script.'''

# TODO: allow different python modules to add their own args - instead of a centralized location. This should allow for better Hyperdeck / Automaton reusability.

import __main__
import argparse
import os

parser = argparse.ArgumentParser(
    description=__main__.__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--fresh', action='store_true')
parser.add_argument('--release', action='store_true')
parser.add_argument('--debug', action='store_true')
parser.add_argument('--live', action='store_true')
parser.add_argument('target')
args = parser.parse_args()

for k, v in args.__dict__.items():
  globals()[k] = v
