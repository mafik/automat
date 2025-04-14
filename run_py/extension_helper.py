# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import build
import cmake
import git
import ninja
import re
import src
import os
import fs_utils
from collections import defaultdict
from functools import partial
from make import Popen
from pathlib import Path
from typing import Callable

def download_from_url(url : str, filename : Path):
    import os
    import urllib.request
    
    # Create parent directory if it doesn't exist
    os.makedirs(filename.parent, exist_ok=True)
    
    # Download the file from url
    urllib.request.urlretrieve(url, filename)

def extract_tar(archive : Path, output : Path):
  import tarfile
  import os
  import shutil

  tmp_dir = output / 'tmp'
  os.makedirs(tmp_dir, exist_ok=True)

  if archive.name.endswith('.tar.gz'):
    tar = tarfile.open(archive, 'r:gz')
  elif archive.name.endswith('.tar.xz'):
    tar = tarfile.open(archive, 'r:xz')
  else:
    raise ValueError(f'Unknown archive format for {archive} (supported: .tar.gz, .tar.xz)')
  tar.extractall(tmp_dir)
  children = list(tmp_dir.iterdir())

  # if the archive contained a single directory, move its contents to the output
  if len(children) == 1 and children[0].is_dir():
    children = list(children[0].iterdir())
  for child in children:
    shutil.move(child, output)

  shutil.rmtree(tmp_dir)


class ExtensionHelper:
  '''
  Helper for extensions that build third party libraries.
  
  Adds the steps to build the library and modifyies the Automat compilation
  graph to link the library correctly.
  '''

  # General flow of steps:
  # - fetch (git / http)
  # - configure (cmake / meson)
  # - build & install (ninja)
  # - post-install hooks
  # - (inject deps & compile args) compile object files with sources that match include_regex
  # - (inject link args) link binaries

  '''Pass in the name of the library being built and the `globals()` of the extension module'''
  def __init__(self, name, module_globals):
    self.name = name
    self.module_globals = module_globals
    self.old_hook_recipe = module_globals.get('hook_recipe', None)
    self.old_hook_srcs = module_globals.get('hook_srcs', None)
    self.old_hook_plan = module_globals.get('hook_plan', None)
    self.module_globals['hook_recipe'] = self._hook_recipe
    self.module_globals['hook_srcs'] = self._hook_srcs
    self.module_globals['hook_plan'] = self._hook_plan
    self.configure = None
    self.configure_opts = dict()
    self.install_srcs = set()
    self.install_objs = set()
    self.install_bins = set()
    self.compile_args = []
    self.link_args = []
    self.run_args = []
    self.outputs = defaultdict(list) # holds a list of outputs for each build type
    # beam is a list of files that are required for the next step in the build
    # it's used to conditionally inject steps in the build graph
    self.beam = defaultdict(list)
    self.patch_sources_func = None
    self.post_install = None
    self.post_install_outputs = defaultdict(list)
    self.checkout_dir = fs_utils.third_party_dir / self.name
    self.src_dir = self.checkout_dir
    self.git_url = None
    self.git_tag = None
    self.fetch_url = None
    self.fetch_filename = None
    self.include_regex = None
    self.skip_configure = False
    self.configure_inputs = defaultdict(list)

  def _hook_recipe(self, recipe):
    if self.old_hook_recipe:
      self.old_hook_recipe(recipe)

    if self.git_url:
      recipe.add_step(
          git.clone(self.git_url, self.checkout_dir, self.git_tag),
          outputs=[self.checkout_dir],
          inputs=[],
          desc = f'Downloading {self.name}',
          shortcut=f'get {self.name}')
      self.beam[''] = [self.checkout_dir]

    elif self.fetch_url:
      archive = fs_utils.third_party_dir / self.fetch_filename
      recipe.add_step(
          partial(download_from_url, self.fetch_url, archive),
          outputs=[archive],
          inputs=[],
          desc = f'Downloading {self.name}',
          shortcut=f'get {self.name}')
      
      if archive.name.endswith('.tar.gz') or archive.name.endswith('.tar.xz'):
        recipe.add_step(
            partial(extract_tar, archive, self.checkout_dir),
            outputs=[self.checkout_dir],
            inputs=[archive],
            desc = f'Extracting {self.name}',
            shortcut=f'extract {self.name}')
      else:
        raise ValueError(f'Unknown archive format for {archive} (supported: .tar.gz, .tar.xz)')
      self.beam[''] = [self.checkout_dir]

    if self.patch_sources_func:
      marker = fs_utils.third_party_dir / f'{self.name}.patched'
      recipe.add_step(
          partial(self.patch_sources_func, marker),
          outputs=[marker],
          inputs=self.beam[''],
          desc = f'Patching {self.name}',
          shortcut=f'patch {self.name}')
      self.beam[''] = [marker]

    if self.skip_configure:
      for build_type in build.types:
        self.beam[build_type.name] = self.beam['']
      return

    for build_type in build.types:
      build_dir = build_type.BASE() / self.name

      configure_inputs = [self.module_globals['__file__'], __file__] + self.beam[''] + self.configure_inputs[build_type.name]
      makefile = None

      if self.configure == 'cmake':
        cmake_args = cmake.CMakeArgs(build_type, self.configure_opts)
        makefile = build_dir / 'build.ninja'
        recipe.add_step(
            partial(Popen, cmake_args +
                              ['-S', str(self.src_dir).replace('\\', '/'), '-B', str(build_dir).replace('\\', '/')]),
            outputs=[makefile],
            inputs=configure_inputs,
            desc=f'Configuring {self.name} {build_type}'.strip(),
            shortcut=f'configure {self.name} {build_type}'.strip())
    
      elif self.configure == 'meson':
        meson_args = ['meson', 'setup']
        for key, value in self.configure_opts.items():
          meson_args.append(f'-D{key}={value}')
        if build_type.is_subtype_of(build.fast):
          meson_args += ['--buildtype=debugoptimized']
        elif build_type.is_subtype_of(build.debug):
          meson_args += ['--buildtype=debug']
        elif build_type.is_subtype_of(build.release):
          meson_args += ['--buildtype=release']
        meson_args += ['--default-library=both', '--prefix', build_type.PREFIX(), '--libdir', 'lib64', build_dir, self.src_dir]
        makefile = build_dir / 'build.ninja'

        recipe.add_step(
          partial(Popen, meson_args),
          outputs=[makefile],
          inputs=configure_inputs,
          desc=f'Configuring {self.name} {build_type}',
          shortcut=f'configure {self.name} {build_type}')
      
      elif self.configure == 'autotools':
        makefile = build_dir / 'Makefile'

        def configure(build_type=build_type, build_dir=build_dir):
          build_dir.mkdir(parents=True, exist_ok=True)
          prefix = build_type.PREFIX()
          env = os.environ.copy()
          env['PKG_CONFIG_PATH'] = f'{prefix}/share/pkgconfig:{prefix}/lib64/pkgconfig'
          env['CC'] = build.compiler_c
          env['CFLAGS'] = ' '.join(f for f in build_type.CFLAGS() if not f.startswith('-Werror'))
          print('Flags', env['CFLAGS'])
          return Popen([(self.src_dir / 'configure').absolute(), f'--prefix={prefix}', f'--libdir={prefix}/lib64', '--disable-shared'], env=env, cwd=build_dir)
    
        recipe.add_step(
            configure,
            outputs=[makefile],
            inputs=configure_inputs,
            desc=f'Configuring {self.name} {build_type}',
            shortcut=f'configure {self.name} {build_type}')

      else:
        raise ValueError(f'{self.name} is not configured')
      
      if makefile.name == 'build.ninja':
        recipe.add_step(
          partial(Popen, [ninja.BIN, '-C', build_dir, 'install']),
          outputs=self.outputs[build_type.name],
          inputs=[build_dir / 'build.ninja', ninja.BIN],
          desc=f'Installing {self.name} {build_type}',
          shortcut=f'install {self.name} {build_type}')
      elif makefile.name == 'Makefile':
        recipe.add_step(
            partial(Popen, ['make', 'install', '-j', '8'], cwd=build_dir),
            outputs=self.outputs[build_type.name],
            inputs=[build_dir / 'Makefile'],
            desc=f'Installing {self.name} {build_type}',
            shortcut=f'install {self.name} {build_type}')
      else:
        raise ValueError(f'Unknown build system for {self.name} {build_type}')

      self.beam[build_type.name] = self.outputs[build_type.name]
    
      if self.post_install:
        recipe.add_step(partial(self.post_install, build_type, *self.post_install_outputs[build_type]),
                        outputs=self.post_install_outputs[build_type],
                        inputs=self.beam[build_type.name],
                        desc=f'Post-install hook for {self.name} {build_type}'.strip(),
                        shortcut=f'post-install {self.name} {build_type}'.strip())
        self.beam[build_type.name] = self.post_install_outputs[build_type]

  def _hook_srcs(self, srcs : dict[str, src.File], recipe):
    if self.old_hook_srcs:
      self.old_hook_srcs(srcs, recipe)
    if not self.include_regex:
      return
    for src in srcs.values():
      if any(self.include_regex.match(inc) for inc in src.system_includes):
        self.install_srcs.add(src)

  def _hook_plan(self, srcs, objs : list[build.ObjectFile], bins, recipe):
    if self.old_hook_plan:
      self.old_hook_plan(srcs, objs, bins, recipe)
    for obj in objs:
      if obj.deps.intersection(self.install_srcs):
        self.install_objs.add(obj)
        obj.deps.update(self.beam[obj.build_type.name])
        obj.compile_args += self.compile_args

    for bin in bins:
      if self.install_objs.intersection(bin.objects):
        self.install_bins.add(bin)
        bin.link_args += self.link_args
        bin.run_args += self.run_args

  '''Can be used to delay the configure step until some other files are ready.
  
  `dep` can be a:
   - string with a {PREFIX} placeholder
   - function that takes a BuildType and returns a list of files to wait for
   - another ExtensionHelper'''
  def ConfigureDependsOn(self, *deps):
    for dep in deps:
      as_build_type_to_path_dict(dep, self.configure_inputs)

  def FetchFromGit(self, git_url, git_tag):
    if self.git_url:
      raise ValueError(f'{self.name} already has git URL set')
    if self.fetch_url:
      raise ValueError(f'{self.name} already has fetch URL set')
    self.git_url = git_url
    self.git_tag = git_tag

  def FetchFromURL(self, fetch_url, fetch_filename=None):
    if self.git_url:
      raise ValueError(f'{self.name} already has git URL set')
    if self.fetch_url:
      raise ValueError(f'{self.name} already has fetch URL set')
    self.fetch_url = fetch_url
    if fetch_filename:
      self.fetch_filename = fetch_filename
    else:
      self.fetch_filename = fetch_url.split('/')[-1]

  def SetSrcDir(self, src_dir):
    self.src_dir = src_dir

  def SkipConfigure(self):
    self.skip_configure = True

  def ConfigureWithCMake(self, *outputs):
    '''
      output: function that takes a BuildType and returns a path to the output of `ninja install`.
    '''
    if self.configure:
      raise ValueError(f'{self.name} was already configured')
    self.configure = 'cmake'
    as_build_type_to_path_dict(outputs, self.outputs)
  
  def ConfigureWithMeson(self, *outputs):
    if self.configure:
      raise ValueError(f'{self.name} was already configured')
    self.configure = 'meson'
    as_build_type_to_path_dict(outputs, self.outputs)

  def ConfigureWithAutotools(self, *outputs):
    if self.configure:
      raise ValueError(f'{self.name} was already configured')
    self.configure = 'autotools'
    as_build_type_to_path_dict(outputs, self.outputs)

  def ConfigureOption(self, name:str, value:str):
    '''
    Sets an option that will be passed when configuring the build.

    For CMake & Meson it's going to be passed as a -D<name>=<value> argument.
    '''
    if name in self.configure_opts:
      raise ValueError(f'Configuration option {name} was already defined')
    self.configure_opts[name] = value

  def ConfigureOptions(self, **kwargs):
    for name, value in kwargs.items():
      self.ConfigureOption(name, value)

  def InstallWhenIncluded(self, include_regex):
    if self.include_regex:
      raise ValueError(f'{self.name} was already configured with include regex')
    self.include_regex = re.compile(include_regex)

  def AddCompileArg(self, compile_arg):
    '''Argument may be callable - it will be invoked with the build type as an argument at compilation time'''
    self.compile_args.append(compile_arg)

  def AddCompileArgs(self, *compile_args):
    for compile_arg in compile_args:
      self.AddCompileArg(compile_arg)

  def AddLinkArg(self, link_arg):
    '''Argument may be callable - it will be invoked with the build type as an argument at link time'''
    self.link_args.append(link_arg)

  def AddLinkArgs(self, *link_args):
    for link_arg in link_args:
      self.AddLinkArg(link_arg)

  def AddRunArg(self, run_arg):
    self.run_args.append(run_arg)

  def AddRunArgs(self, run_args):
    for run_arg in run_args:
      self.AddRunArg(run_arg)

  def PatchSources(self, func : Callable[[Path], None]):
    if self.patch_sources_func:
      raise ValueError(f'{self.name} was already configured with patch sources hook')
    self.patch_sources_func = func

  def PostInstallStep(self, func : Callable[[build.BuildType, Path], None], outputs_func : Callable[[build.BuildType], list[Path]] = None):
    '''
    Run the given function after installation.
    
    The post-install function must produce some files to mark its completion. They can be provided through the `outputs` function.
    If no outputs_func is provided, a default marker file will be used.
    
    `func` takes the build type and a list (as varargs) of output files.
    '''
    if self.post_install:
      raise ValueError(f'{self.name} was already configured with post-install hook')
    self.post_install = func
    
    if outputs_func:
      as_build_type_to_path_dict(outputs_func, self.post_install_outputs)
    else:
      for build_type in build.types:
        self.post_install_outputs[build_type] = [build_type.BASE() / f'{self.name}.install_patched']


def as_build_type_to_path_dict(arg, dict_to_fill=defaultdict(list)):
  '''Converts a function / string / list / ExtensionHelper to a dict that maps BuildType to Path'''
  
  if isinstance(arg, list) or isinstance(arg, tuple):
    for a in arg:
      as_build_type_to_path_dict(a, dict_to_fill)
  elif callable(arg):
    for build_type in build.types:
      ret = arg(build_type)
      if isinstance(ret, list):
        dict_to_fill[build_type.name] += ret
      elif isinstance(ret, Path) or isinstance(ret, str):
        dict_to_fill[build_type.name].append(ret)
      else:
        raise ValueError(f'Function {arg} returned {type(ret)} but expected Path or str (or list of those)')
  elif isinstance(arg, str):
    for build_type in build.types:
      dict_to_fill[build_type.name].append(arg.format(PREFIX=build_type.PREFIX()))
  elif isinstance(arg, dict):
    for k, v in arg.items():
      dict_to_fill[k] += v
  elif isinstance(arg, ExtensionHelper):
    source_dict = arg.outputs
    if arg.post_install:
      source_dict = arg.post_install_outputs
    for k, v in source_dict.items():
      dict_to_fill[k] += v
  else:
    raise ValueError(f'Unknown output type: {type(arg)}')
  return dict_to_fill