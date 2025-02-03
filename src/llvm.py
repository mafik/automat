# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import subprocess
import build
import extension_helper

hook = extension_helper.ExtensionHelper('LLVM', globals())

def llvm_config_path(build_type : build.BuildType):
  return (build_type.PREFIX() / 'bin' / 'llvm-config').with_suffix(build.binary_extension)

def llvm_config(build_type : build.BuildType, args):
  '''Runs llvm-config for the given build_type and returns the output as a list of strings'''
  if '"' in args or "'" in args or '\\' in args:
    raise ValueError("TODO: handle potential shell escaping in llvm-config output")
  llvm_config = llvm_config_path(build_type)
  if not llvm_config.exists():
    return []
  stdout_bytes = subprocess.Popen([llvm_config] + args, stdout=subprocess.PIPE).stdout.read()
  return stdout_bytes.decode().split()

def llvm_cxxflags(build_type : build.BuildType):
  flags = llvm_config(build_type, ['--cxxflags'])
  flags = [f for f in flags if not f.startswith('-std=')]
  x86_include = build_type.BASE() / 'LLVM' / 'lib' / 'Target' / 'X86'
  flags.append(f'-I{x86_include}')
  return flags

def llvm_ldflags(build_type : build.BuildType):
  return llvm_config(build_type, ['--ldflags', '--libs']) + ['-lz', '-lzstd']

def post_install(build_type : build.BuildType, marker):
  llvm_lib = build_type.PREFIX() / 'include' / 'llvm' / 'lib'
  if llvm_lib.exists() and llvm_lib.is_symlink():
    llvm_lib.unlink()
  llvm_lib.symlink_to(hook.checkout_dir / 'llvm' / 'lib')
  marker.touch()

hook.FetchFromGit('https://github.com/llvm/llvm-project.git', 'llvmorg-19.1.6')
hook.ConfigureWithCMake(src_dir=hook.checkout_dir / 'llvm', output=llvm_config_path)
hook.ConfigureOption('LLVM_ENABLE_RTTI', 'ON')
hook.ConfigureOption('LLVM_TARGETS_TO_BUILD', 'X86')
hook.ConfigureOption('LLVM_USE_LINKER', 'lld')
hook.ConfigureOption('LLVM_INCLUDE_BENCHMARKS', 'OFF') # not needed
hook.ConfigureOption('LLVM_INCLUDE_EXAMPLES', 'OFF') # not needed
hook.ConfigureOption('LLVM_INCLUDE_TESTS', 'OFF') # not needed
hook.ConfigureOption('LLVM_OPTIMIZED_TABLEGEN', 'ON') # we're not including tablegen in Autmat, it can be always optimized
# LTO is disabled because it's using too much memory and is crashing the build
# hook.ConfigureOption('LLVM_ENABLE_LTO', 'ON')
hook.ConfigureOption('LLVM_RAM_PER_LINK_JOB', '10000')
if build.platform == 'win32':
  hook.ConfigureOption('LLVM_HOST_TRIPLE', 'x86_64-pc-windows') # needed to build on Windows
hook.ConfigureOption('CMAKE_CXX_STANDARD', '20')
hook.InstallWhenIncluded(r'llvm/.*\.h')
hook.AddCompileArg(llvm_cxxflags)
hook.AddLinkArg(llvm_ldflags)
hook.PatchInstallation(post_install)
