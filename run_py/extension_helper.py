# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import build
import cmake
import git
import ninja
import re
import src
import fs_utils
from collections import defaultdict
from functools import partial
from make import Popen
from pathlib import Path
from typing import Callable

class ExtensionHelper:
  '''
  Helper for extensions that build third party libraries.
  
  Adds the steps to build the library and modifyies the Automat compilation
  graph to link the library correctly.
  '''

  # General flow of steps:
  # - fetch from git
  # - configure (cmake)
  # - build & install (ninja)
  # - post-install hooks
  # - (inject deps & compile args) compile object files with sources that match include_regex
  # - (inject link args) link binaries

  '''Pass in the name of the library being built and the `globals()` of the extension module'''
  def __init__(self, name, module_globals):
    self.name = name
    self.module_globals = module_globals
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
    self.outputs = dict()
    # beam is a list of files that are required for the next step in the build
    # it's used to conditionally inject steps in the build graph
    self.beam = defaultdict(list)
    self.patch_sources_func = None
    self.post_install = None
    self.checkout_dir = fs_utils.third_party_dir / self.name
    self.src_dir = self.checkout_dir
    self.git_url = None
    self.git_tag = None

  def _hook_recipe(self, recipe):

    if self.git_url:
      recipe.add_step(
          git.clone(self.git_url, self.checkout_dir, self.git_tag),
          outputs=[self.checkout_dir],
          inputs=[],
          desc = f'Downloading {self.name}',
          shortcut=f'get {self.name}')
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

    for build_type in build.types:
      build_dir = build_type.BASE() / self.name
      if self.configure == 'cmake':
        cmake_args = cmake.CMakeArgs(build_type, self.configure_opts)
        recipe.add_step(
            partial(Popen, cmake_args +
                              ['-S', self.src_dir, '-B', build_dir]),
            outputs=[build_dir / 'build.ninja'],
            inputs=self.beam[''] + [self.module_globals['__file__']],
            desc=f'Configuring {self.name} {build_type}'.strip(),
            shortcut=f'configure {self.name} {build_type}'.strip())
        
        self.beam[build_type.name] = [self.outputs[build_type.name]]
    
      elif self.configure == 'meson':
        build_dir = build_type.BASE() / self.name
        meson_args = ['meson', 'setup']
        for key, value in self.configure_opts.items():
          meson_args.append(f'-D{key}={value}')
        meson_args += ['--default-library=both', '--prefix', build_type.PREFIX(), '--libdir', 'lib64', build_dir, self.src_dir]

        recipe.add_step(
          partial(Popen, meson_args),
          outputs=[build_dir / 'build.ninja'],
          inputs=self.beam[''],
          desc=f'Configuring {self.name} {build_type}',
          shortcut=f'configure {self.name} {build_type}')
      else:
        raise ValueError(f'{self.name} is not configured')
        
      recipe.add_step(
        partial(Popen, [ninja.BIN, '-C', build_dir, 'install']),
        outputs=[self.outputs[build_type.name]],
        inputs=[build_dir / 'build.ninja', ninja.BIN],
        desc=f'Installing {self.name} {build_type}',
        shortcut=f'install {self.name} {build_type}')

      self.beam[build_type.name] = [self.outputs[build_type.name]]
    
      if self.post_install:
        marker = build_type.BASE() / f'{self.name}.install_patched'
        recipe.add_step(partial(self.post_install, build_type, marker),
                        outputs=[marker],
                        inputs=self.beam[build_type.name],
                        desc=f'Patching installed {self.name} {build_type}'.strip(),
                        shortcut=f'patch {self.name} {build_type}'.strip())
        self.beam[build_type.name] = [marker]

  def _hook_srcs(self, srcs : dict[str, src.File], recipe):
    for src in srcs.values():
      if any(self.include_regex.match(inc) for inc in src.system_includes):
        self.install_srcs.add(src)

  def _hook_plan(self, srcs, objs, bins, recipe):
    for obj in objs:
      if obj.deps.intersection(self.install_srcs):
        self.install_objs.add(obj)
        obj.deps.update(self.beam[obj.build_type.name])
        obj.compile_args += self.compile_args

    for bin in bins:
      if self.install_objs.intersection(bin.objects):
        self.install_bins.add(bin)
        bin.link_args += self.link_args
    

  def FetchFromGit(self, git_url, git_tag):
    if self.git_url:
      raise ValueError(f'{self.name} already has git URL set')
    self.git_url = git_url
    self.git_tag = git_tag

  def ConfigureWithCMake(self, src_dir, output):
    '''
      output: function that takes a BuildType and returns a path to the output of `ninja install`.
    '''
    if self.configure:
      raise ValueError(f'{self.name} was already configured')
    self.configure = 'cmake'
    self.src_dir = src_dir
    for build_type in build.types:
      self.outputs[build_type.name] = output(build_type)
  
  def ConfigureWithMeson(self, output):
    if self.configure:
      raise ValueError(f'{self.name} was already configured')
    self.configure = 'meson'
    for build_type in build.types:
      self.outputs[build_type.name] = output(build_type)

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
    if hasattr(self, 'include_regex'):
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

  def PatchSources(self, func : Callable[[Path], None]):
    if self.patch_sources_func:
      raise ValueError(f'{self.name} was already configured with patch sources hook')
    self.patch_sources_func = func

  def PatchInstallation(self, func : Callable[[build.BuildType, Path], None]):
    '''
    Run the given function after installation. `func` takes the build type and a marker that should be touched after patching.
    '''
    if self.post_install:
      raise ValueError(f'{self.name} was already configured with post-install hook')
    self.post_install = func
