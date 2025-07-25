# SPDX-FileCopyrightText: Copyright 2025 Automat Authors
# SPDX-License-Identifier: MIT

import subprocess
import build
import sys
import extension_helper
import fs_utils

hook = extension_helper.ExtensionHelper('LLVM', globals())

llvm_config_path = (build.PREFIX / 'bin' / 'llvm-config').with_suffix(fs_utils.binary_extension)

def llvm_cxxflags():
  path = build.BASE / 'LLVM.cxxflags'
  if path.exists():
    return path.read_text().split()
  return []

def llvm_ldflags():
  path = build.BASE / 'LLVM.ldflags'
  if path.exists():
    return path.read_text().split()
  return []

def PostInstallProcess(BASE, PREFIX, checkout_dir):
  from pathlib import Path
  import subprocess
  import fs_utils

  BASE = Path(BASE)
  PREFIX = Path(PREFIX)
  checkout_dir = Path(checkout_dir)

  llvm_lib = PREFIX / 'include' / 'llvm' / 'lib'
  if llvm_lib.exists() and llvm_lib.is_symlink():
    llvm_lib.unlink()
  llvm_lib.symlink_to(checkout_dir / 'llvm' / 'lib')

  ldflags_path = BASE / 'LLVM.ldflags'
  cxxflags_path = BASE / 'LLVM.cxxflags'

  llvm_config = (PREFIX / 'bin' / 'llvm-config').with_suffix(fs_utils.binary_extension)
  llvm_config_ldflags = subprocess.Popen([llvm_config, '--ldflags', '--libs'], stdout=ldflags_path.open('w'))
  llvm_config_cxxflags = subprocess.Popen([llvm_config, '--cxxflags'], stdout=cxxflags_path.open('w'))
  llvm_config_ldflags.wait()
  llvm_config_cxxflags.wait()

  with ldflags_path.open('a') as f:
    if fs_utils.platform == 'win32':
      f.write('-lntdll')
    else:
      f.write('-lz -lzstd')

  cxxflags = cxxflags_path.read_text().split()
  cxxflags = [f for f in cxxflags if not f.startswith('-std=')]
  extra_dir = BASE / 'LLVM' / 'lib' / 'Target' / 'X86'
  cxxflags.append(f'-I{extra_dir}')
  cxxflags_path.write_text(' '.join(cxxflags))

# We need this in order to isolate the build system from extra
# processes (llvm-config) required to find the LLVM arguments.
#
# This was originally done with multiprocessing bit it had issues
# on Windows.
def RunFunctionAsProcess(func, *args):
  import inspect
  source = inspect.getsource(func)

  script = source
  script += '\n\n'
  script += func.__name__ + '(' + ', '.join([repr(arg) for arg in args]) + ')'
  p = subprocess.Popen([sys.executable], stdin=subprocess.PIPE, env={ 'PYTHONPATH': str(fs_utils.run_py_dir) })
  p.stdin.write(script.encode('utf-8'))
  p.stdin.close()
  return p

def post_install(*outputs):
  return RunFunctionAsProcess(PostInstallProcess,
                              str(build.BASE),
                              str(build.PREFIX),
                              str(hook.checkout_dir))

hook.FetchFromGit('https://github.com/llvm/llvm-project.git', 'llvmorg-19.1.6')
hook.SetSrcDir(hook.checkout_dir / 'llvm')
hook.ConfigureWithCMake(llvm_config_path)
hook.ConfigureOption('LLVM_ENABLE_RTTI', 'ON')
hook.ConfigureOption('LLVM_TARGETS_TO_BUILD', 'X86')
hook.ConfigureOption('LLVM_USE_LINKER', 'lld')
hook.ConfigureOption('LLVM_INCLUDE_BENCHMARKS', 'OFF') # not needed
hook.ConfigureOption('LLVM_INCLUDE_EXAMPLES', 'OFF') # not needed
hook.ConfigureOption('LLVM_INCLUDE_TESTS', 'OFF') # not needed

# Normally this should be always ON, but on Windows CMake cannot properly guess
# the host triple for the debug build (specifically the NATIVE sub-build).
if build.platform == 'win32':
  hook.ConfigureOption('LLVM_OPTIMIZED_TABLEGEN', 'OFF')
else:
  hook.ConfigureOption('LLVM_OPTIMIZED_TABLEGEN', 'ON')

# LTO is disabled because it's using too much memory and is crashing the build
# hook.ConfigureOption('LLVM_ENABLE_LTO', 'ON')
hook.ConfigureOption('LLVM_RAM_PER_LINK_JOB', '10000')
if build.platform == 'win32':
  hook.ConfigureOption('LLVM_HOST_TRIPLE', 'x86_64-pc-windows') # needed to build on Windows
hook.ConfigureOption('CMAKE_CXX_STANDARD', '17')
hook.InstallWhenIncluded(r'llvm/.*\.h')
hook.AddCompileArg(llvm_cxxflags)
hook.AddLinkArg(llvm_ldflags)

hook.PostInstallStep(post_install,
                     [build.BASE / 'LLVM.ldflags',
                      build.BASE / 'LLVM.cxxflags'])
