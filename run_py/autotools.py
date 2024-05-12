from functools import partial
from sys import platform

import cmake
import fs_utils
import build
import os
import make
import re

Popen = make.Popen

package_name_re = re.compile(r'(?P<name>[A-Za-z0-9_\-]+)-(?P<version>[0-9.]+)\.tar\.(?:gz|xz|bz2)')

# Adds the given package to the recipe build graph
def register_package(recipe, url, inputs=[], outputs=[]):
  filename = url.split('/')[-1]
  match = package_name_re.match(filename)
  if not match:
    raise ValueError(f'Could not parse package name from: {filename}')
  name = match.group('name')
  version = match.group('version')
  source_dir = fs_utils.build_dir / f'{name}-{version}'
  tarball = fs_utils.build_dir / filename

  recipe.add_step(
      partial(Popen, ['curl', '-L', url, '-o', tarball]),
      outputs=[tarball],
      inputs=[],
      desc = f'Downloading {name}',
      shortcut=f'download {name}')
  
  recipe.add_step(
      partial(Popen, ['tar', 'xf', tarball, '-C', fs_utils.build_dir]),
      outputs=[source_dir],
      inputs=[tarball],
      desc = f'Extracting {name}',
      shortcut=f'extract {name}')

  for build_type in build.types:
    build_dir = source_dir / 'build' / build_type.name
    prefix = build_type.PREFIX()
    build_inputs = [i.format(PREFIX=prefix) for i in inputs]
    build_outputs = [o.format(PREFIX=prefix) for o in outputs]

    def configure(build_type=build_type, build_dir=build_dir, prefix=prefix):
      build_dir.mkdir(parents=True, exist_ok=True)

      env = os.environ.copy()
      env['PKG_CONFIG_PATH'] = f'{prefix}/share/pkgconfig:{prefix}/lib/pkgconfig'
      env['CC'] = build.compiler_c
      env['CFLAGS'] = ' '.join(build_type.CFLAGS())
      return Popen([(source_dir / 'configure').absolute(), '--prefix', prefix], env=env, cwd=build_dir)
    
    recipe.add_step(
        configure,
        outputs=[build_dir / 'Makefile'],
        inputs=build_inputs + [source_dir],
        desc=f'Configuring {name}{build_type.rule_suffix()}',
        shortcut=f'configure {name}{build_type.rule_suffix()}')
    
    recipe.add_step(
        partial(Popen, ['make', 'install', '-j', '8'], cwd=build_dir),
        outputs=build_outputs,
        inputs=[build_dir / 'Makefile'],
        desc=f'Building {name}{build_type.rule_suffix()}',
        shortcut=f'{name}{build_type.rule_suffix()}')
