from collections import defaultdict
from functools import partial
import multiprocessing
import subprocess
import cmake
import fs_utils
import build
import os
import make
import re
import src

PIPEWIRE_OPTIONS = {
  'spa-plugins': 'disabled',
  'alsa': 'disabled',
  'pipewire-alsa': 'disabled',
  'systemd': 'disabled',
  'gstreamer': 'disabled',
  'tests': 'disabled',
  'libusb': 'disabled',
  'libpulse': 'disabled',
  'pw-cat': 'disabled',
  'sndfile': 'disabled',
  'dbus': 'disabled',
  'flatpak': 'disabled',
  'session-managers': '[]',
}

PIPEWIRE_ROOT = fs_utils.third_party_dir / 'PipeWire'

PATCH_INPUT = PIPEWIRE_ROOT / 'src' / 'pipewire' / 'meson.build'
PATCH_MARKER = PIPEWIRE_ROOT / 'src' / 'pipewire' / 'meson.build.patched'

def patch_meson_build():
  # replace "libpipewire = shared_library" with "libpipewire = library"
  meson_build = PATCH_INPUT
  lines = open(meson_build).readlines()
  with open(meson_build, 'w') as f:
    for line in lines:
      f.write(re.sub(r'libpipewire = shared_library', 'libpipewire = library', line))
  PATCH_MARKER.touch()
    
class LibraryAutolinker:
  '''Automatically recognize if any file uses the given library and link it properly.'''

  def __init__(self, include_regex):
    self.regex = re.compile(include_regex)
    self.inputs = defaultdict(list)
    self.compile_args = defaultdict(list)
    self.link_args = defaultdict(list)

  def hook_srcs(self, srcs : dict[str, src.File], recipe):
    self.srcs = set()
    for src in srcs.values():
      if any(self.regex.match(inc) for inc in src.system_includes):
        self.srcs.add(src)

  def hook_plan(self, srcs, objs : list[build.ObjectFile], bins : list[build.Binary], recipe : make.Recipe):
    self.objs = set()
    for obj in objs:
      if obj.source in self.srcs:
        for dep in self.inputs[obj.build_type.name]:
          obj.deps.add(dep)
        for arg in self.compile_args['']:
          obj.compile_args.append(arg)
        for arg in self.compile_args[obj.build_type.name]:
          obj.compile_args.append(arg)
        self.objs.add(obj)

    for bin in bins:
      if self.objs.intersection(bin.objects):
        for arg in self.link_args['']:
          bin.link_args.append(arg)
        for arg in self.link_args[bin.build_type.name]:
          bin.link_args.append(arg)

  def hook_final(self, srcs, objs, bins, recipe):
    return

autolinker = LibraryAutolinker('pipewire/pipewire.h')

def hook_recipe(recipe):
  recipe.add_step(
      partial(make.Popen, ['git', 'clone', '--depth', '1', 'https://gitlab.freedesktop.org/pipewire/pipewire.git', PIPEWIRE_ROOT]),
      outputs=[PATCH_INPUT],
      inputs=[],
      desc = 'Downloading PipeWire',
      shortcut='get pipewire')
  
  recipe.add_step(patch_meson_build,
      outputs=[PATCH_MARKER],
      inputs=[PATCH_INPUT],
      desc = 'Patching meson.build',
      shortcut='patch pipewire meson.build')
  
  autolinker.compile_args[''] += ['-D_REENTRANT']
  autolinker.link_args[''] += ['-ldl', '-lm', '-pthread', '-lpipewire-0.3']
  for build_type in build.types:
    build_dir = fs_utils.build_dir / 'pipewire' / build_type.name
    opts = []
    for key, value in PIPEWIRE_OPTIONS.items():
      opts.append(f'-D{key}={value}')
    recipe.add_step(
      partial(make.Popen, ['meson', 'setup'] + opts + ['--default-library=both', '--prefix', str(build_type.PREFIX()), build_dir, PIPEWIRE_ROOT]),
      outputs=[build_dir / 'Makefile'],
      inputs=[PATCH_MARKER],
      desc=f'Configuring PipeWire {build_type}',
      shortcut=f'configure pipewire {build_type}')
    
    install_output = str(build_dir / 'src' / 'pipewire' / 'libpipewire-0.3.a')
    recipe.add_step(
      partial(make.Popen, ['ninja', '-C', build_dir, 'install']),
      outputs=[install_output],
      inputs=[build_dir / 'Makefile'],
      desc=f'Building PipeWire {build_type}',
      shortcut=f'build pipewire {build_type}')
    
    autolinker.inputs[build_type.name] += [install_output]
    autolinker.compile_args[build_type.name] += [f'-I{build_type.PREFIX()}/include/pipewire-0.3', f'-I{build_type.PREFIX()}/include/spa-0.2']


def hook_srcs(srcs, recipe):
  autolinker.hook_srcs(srcs, recipe)

def hook_plan(srcs, objs, bins, recipe):
  autolinker.hook_plan(srcs, objs, bins, recipe)

def hook_final(srcs, objs, bins, recipe):
  autolinker.hook_final(srcs, objs, bins, recipe)
