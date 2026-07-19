# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

# TensorFlow, as upstream's official self-contained shared library on both
# platforms: bazel builds `//tensorflow:libtensorflow.so.2.20.0` on Linux and
# `//tensorflow:tensorflow.dll` on Windows with supported flags only; nothing
# in the TensorFlow tree is written or patched. --config=monolithic folds the
# whole runtime into the one file (a separate framework library would rerun
# its registrars when both halves load), and keeps TF's bundled LLVM and MLIR
# inside it, where they cannot collide with Automat's own LLVM.
#
# The library installs under build/tensorflow, shared by all variants: it is
# always built -c opt and loaded dynamically, so the variant's flags do not
# apply to it. src/tensorflow_runtime.cpp #embeds it and loads it on first
# use - memfd + dlopen on Linux, an extract-once cache file + LoadLibrary on
# Windows. Automat calls the TensorFlow C++ API
# directly, and the calls reach the loaded copy late-bound: on Linux through
# the dlsym trampolines in tensorflow_trampolines.cpp (automat does not link
# the library at all), on Windows through /DELAYLOAD thunks resolved by the
# hook in tensorflow_runtime.cpp.
#
# Automat compiles against the C++ headers in place - the tensorflow source
# tree, the generated headers under bazel-bin, and the absl/Eigen/protobuf/
# tsl/xla/ml_dtypes repos that bazel fetched - so there is no header copy
# step.

import os
import shutil
import subprocess
from pathlib import Path
from sys import platform

import build
import extension_helper
import fs_utils
import make
import src

llvm = src.load_extension('llvm')

CHECKOUT = fs_utils.third_party_dir / 'TensorFlow'
# Shared between variants. bazel fsyncs while extracting repositories, so a
# checkout on a network filesystem would need this pointed at a local disk.
BAZEL_ROOT = fs_utils.build_dir / 'bazel_root'
BAZELISK_DIR = fs_utils.third_party_dir / 'bazelisk'
REPOSITORY_CACHE = fs_utils.third_party_dir / 'bazel_repository_cache'
INSTALL_DIR = fs_utils.build_dir / 'tensorflow'
BAZELISK_VERSION = '1.27.0'
VERSION = '2.20.0'

# TensorFlow's .bazelrc turns on the pywrap (python wheel) packaging mode,
# which does not declare the standalone shared-library targets. The empty
# value selects the classic packaging (the repo rule treats any non-empty
# string, even "False", as true).
PACKAGING_MODE = '--repo_env=USE_PYWRAP_RULES='

if platform == 'win32':
  import ctypes
  import sys
  from glob import glob

  BAZELISK_URL = f'https://github.com/bazelbuild/bazelisk/releases/download/v{BAZELISK_VERSION}/bazelisk-windows-amd64.exe'
  BAZELISK = BAZELISK_DIR / f'bazelisk-{BAZELISK_VERSION}.exe'

  def BazelJobs():
    # The biggest kernel TUs peak around 2 GiB each under clang-cl, so the
    # bazel default of one job per core thrashes; cap concurrency so the peak
    # working set fits in RAM, keeping 2 GiB for the OS and the bazel server.
    class MemoryStatusEx(ctypes.Structure):
      _fields_ = [('dwLength', ctypes.c_uint32), ('dwMemoryLoad', ctypes.c_uint32)] + [
          (name, ctypes.c_uint64)
          for name in ('ullTotalPhys', 'ullAvailPhys', 'ullTotalPageFile', 'ullAvailPageFile',
                       'ullTotalVirtual', 'ullAvailVirtual', 'ullAvailExtendedVirtual')]
    status = MemoryStatusEx(dwLength=ctypes.sizeof(MemoryStatusEx))
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
    return max(1, min(os.cpu_count(), (status.ullTotalPhys - (2 << 30)) // (2 << 30)))

  # The .def file that gives tensorflow.dll its exports is produced by
  # upstream's def_file_filter tool, which appends hardcoded manglings frozen at
  # abseil 2023 / an older protobuf; nothing defines those symbols in TF 2.20
  # and the tensorflow.dll link fails on them. The tool is instantiated as the
  # external repository @local_config_def_file_filter, so a corrected copy
  # (upstream's template, same undname substitution, dead lines dropped) is
  # generated under the build dir and substituted with --override_repository;
  # the TensorFlow tree stays untouched.
  STALE_DEF_EXPORTS = ('lts_20230802', '?Set@ArenaStringPtr', '??0CoordinatedTask')
  FILTER_REPO = build.PREFIX / 'tensorflow_def_file_filter'

  def WriteDefFileFilterRepo():
    tools = CHECKOUT / 'tensorflow' / 'tools' / 'def_file_filter'
    msvc = Path(os.environ['BAZEL_VC']) / 'Tools' / 'MSVC'
    undname = max(msvc.glob('*/bin/HostX64/x64/undname.exe'))
    filter_py = (tools / 'def_file_filter.py.tpl').read_text()
    filter_py = filter_py.replace('%{undname_bin_path}', str(undname).replace('\\', '\\\\'))
    filter_py = filter_py.replace('%{dumpbin_bin_path}',
                                  str(undname.parent / 'dumpbin.exe').replace('\\', '\\\\'))
    kept = [line for line in filter_py.splitlines(keepends=True)
            if not any(stale in line for stale in STALE_DEF_EXPORTS)]
    FILTER_REPO.mkdir(parents=True, exist_ok=True)
    (FILTER_REPO / 'WORKSPACE').write_text('')
    (FILTER_REPO / 'BUILD').write_text((tools / 'BUILD.tpl').read_text())
    (FILTER_REPO / 'def_file_filter.py').write_text(''.join(kept))
    shutil.copyfile(tools / 'symbols_pybind.txt', FILTER_REPO / 'symbols_pybind.txt')

  # clang-cl builds TF; msvc-cl miscompiles TF's bundled LLVM. static_link_msvcrt
  # plus the ucrt linkopts give the DLL the same hybrid CRT automat.exe uses
  # (run_py/build.py): C++ objects cross the module boundary, so both modules
  # must share the ucrt heap.
  BAZEL_FLAGS = ['-c', 'opt', '--config=monolithic', PACKAGING_MODE, '--compiler=clang-cl',
                 '--features=static_link_msvcrt',
                 '--linkopt=/NODEFAULTLIB:libucrt.lib', '--linkopt=/DEFAULTLIB:ucrt.lib',
                 f'--jobs={BazelJobs()}',
                 f'--override_repository=local_config_def_file_filter={FILTER_REPO.as_posix()}',
                 # grpc's C++ TUs include both <windows.h> and clang's <x86intrin.h>,
                 # whose _m_prefetchw definitions clash (C vs C++ linkage); predefine
                 # the include guard so only the SDK declaration is seen.
                 '--per_file_copt=external/com_github_grpc_grpc/.*@-D__PRFCHWINTRIN_H',
                 # The filtered .def keeps no absl names, and delay-load binding needs
                 # every referenced symbol exported; /EXPORT merges with the .def and
                 # flows into the import library. Regenerate like the trampolines: the
                 # symbols tensorflow_runtime.obj leaves undefined that the .def lacks.
                 '--linkopt=/EXPORT:?Unref@StatusRep@status_internal@lts_20250127@absl@@QEBAXXZ']

  # The import library derives from that .def, which exports the core C++ API
  # alongside the C API, so automat's delay-loaded TensorFlow references all
  # resolve against it.
  BAZEL_TARGETS = ['//tensorflow:tensorflow.dll', '//tensorflow:tensorflow_dll_import_lib']
  BUILT = [CHECKOUT / 'bazel-bin' / 'tensorflow' / 'tensorflow.dll',
           CHECKOUT / 'bazel-bin' / 'tensorflow' / 'tensorflow.lib']
  INSTALLED_LIB = INSTALL_DIR / 'tensorflow.dll'
  INSTALLED_IMPLIB = INSTALL_DIR / 'tensorflow.lib'
  INSTALLED = [INSTALLED_LIB, INSTALLED_IMPLIB]
  BAZEL_EXTRA_INPUTS = []

  def BazelEnv():
    # bazel autodetects its clang-cl toolchain from these variables; a missing
    # one produces a cryptic toolchain error, so derive them here and fail
    # loudly only when the underlying tool truly is absent.
    if 'BAZEL_LLVM' not in os.environ:
      clang = shutil.which('clang')
      if clang:
        os.environ['BAZEL_LLVM'] = str(Path(clang).parent.parent)
    if 'BAZEL_SH' not in os.environ:
      for bash in (Path('C:/Program Files/Git/bin/bash.exe'),
                   Path('C:/Program Files/Git/usr/bin/bash.exe')):
        if bash.exists():
          os.environ['BAZEL_SH'] = str(bash)
          break
    if 'BAZEL_VC' not in os.environ:
      candidates = glob('C:/Program Files*/Microsoft Visual Studio/*/*/VC')
      if candidates:
        os.environ['BAZEL_VC'] = max(candidates)
    os.environ.setdefault('PYTHON_BIN_PATH', sys.executable)
    missing = [name for name in ('BAZEL_LLVM', 'BAZEL_SH', 'BAZEL_VC')
               if name not in os.environ]
    if missing:
      raise RuntimeError(
        f'TensorFlow needs {", ".join(missing)} (LLVM, Git bash and VS Build Tools '
        'with the C++ workload); install them or set the variables manually')
    return os.environ.copy()

  def Install():
    for built, installed in zip(BUILT, INSTALLED):
      installed.parent.mkdir(parents=True, exist_ok=True)
      tmp = installed.parent / (installed.name + '.tmp')
      shutil.copyfile(built, tmp)
      tmp.replace(installed)

else:
  BAZELISK_URL = f'https://github.com/bazelbuild/bazelisk/releases/download/v{BAZELISK_VERSION}/bazelisk-linux-amd64'
  BAZELISK = BAZELISK_DIR / f'bazelisk-{BAZELISK_VERSION}'

  def BazelJobs():
    # The biggest MLIR TUs peak around 3 GiB each, so the bazel default of one
    # job per core OOM-kills the bazel server; cap concurrency so the peak
    # working set fits in RAM, keeping 3 GiB for the OS and the bazel server.
    total = os.sysconf('SC_PHYS_PAGES') * os.sysconf('SC_PAGE_SIZE')
    return max(1, min(os.cpu_count(), (total - (3 << 30)) // (3 << 30)))

  # The library hides its C++ API behind upstream's version script; EXPORTS_LDS
  # is a second version-script node returning the tensorflow/tsl/absl symbols
  # automat binds (tensorflow_trampolines.cpp) to the dynamic table.
  EXPORTS_LDS = fs_utils.src_dir / 'tensorflow_exports.lds'
  BAZEL_FLAGS = [
    '-c', 'opt',
    '--config=monolithic',
    PACKAGING_MODE,
    '--force_pic',
    '--spawn_strategy=local',
    f'--jobs={BazelJobs()}',
    f'--linkopt=-Wl,--version-script={EXPORTS_LDS}',
    # clang-20 module layering rejects grpc's zconf.h use.
    '--per_file_copt=external/com_github_grpc_grpc/.*@-Wno-private-header',
  ]

  BAZEL_TARGETS = [f'//tensorflow:libtensorflow.so.{VERSION}']
  BUILT = [CHECKOUT / 'bazel-bin' / 'tensorflow' / f'libtensorflow.so.{VERSION}']
  INSTALLED_LIB = INSTALL_DIR / 'libtensorflow.so'
  INSTALLED = [INSTALLED_LIB]
  BAZEL_EXTRA_INPUTS = [EXPORTS_LDS]

  def BazelEnv():
    env = os.environ.copy()
    env['CC'] = build.compiler_c
    env['CXX'] = build.compiler
    return env

  def Install():
    # Strip the symbol table; the dynamic section, including the C++ exports
    # EXPORTS_LDS added, survives. It more than halves what #embed carries.
    # Write-and-rename: the shared destination must never hold a torn file.
    INSTALLED_LIB.parent.mkdir(parents=True, exist_ok=True)
    tmp = INSTALLED_LIB.parent / (INSTALLED_LIB.name + '.tmp')
    subprocess.run([build.PREFIX / 'bin' / 'llvm-strip', '--strip-all', '-o', tmp,
                    BUILT[0]], check=True)
    tmp.replace(INSTALLED_LIB)


def FetchBazelisk():
  extension_helper.download_from_url(BAZELISK_URL, BAZELISK)
  if platform != 'win32':
    BAZELISK.chmod(0o755)


def BazelBuild():
  env = BazelEnv()
  env['BAZELISK_HOME'] = str(BAZELISK_DIR)
  env['PIP_CACHE_DIR'] = str(BAZEL_ROOT / 'pip_cache')
  if platform == 'win32':
    WriteDefFileFilterRepo()
  return make.Popen(
    [BAZELISK, f'--output_user_root={BAZEL_ROOT}', 'build',
     f'--repository_cache={REPOSITORY_CACHE}'] + BAZEL_FLAGS + BAZEL_TARGETS,
    env=env, cwd=CHECKOUT)


# The C++ API's public headers live in these bazel-fetched repos, alongside the
# tensorflow source tree and its generated headers.
INCLUDE_REPOS = ['com_google_absl', 'eigen_archive', 'com_google_protobuf/src', 'local_tsl',
                 'local_xla', 'ml_dtypes_py']


def IncludeArgs(*_):
  # Each dependency repo has two header roots: the fetched source under
  # <output_base>/external and bazel's generated output (the .pb.h protobuf
  # headers) under bazel-bin/external.
  bin_dir = (CHECKOUT / 'bazel-bin').resolve()
  source_external = bin_dir.parents[4] / 'external'
  gen_external = bin_dir / 'external'
  roots = [CHECKOUT, bin_dir]
  for repo in INCLUDE_REPOS:
    roots += [source_external / repo, gen_external / repo]
  return [f'-I{root}' for root in roots if root.exists()]


SRC_EMBED = fs_utils.src_dir / 'tensorflow_embed.c'


def hook_plan(srcs, objs, bins, recipe):
  for obj in objs:
    if obj.source.path == SRC_EMBED:
      # Wired here rather than through install_srcs because the extension's
      # other compile args (-std=gnu++17, the include roots) are C++-only.
      obj.deps.update(hook.beam)
      obj.compile_args.append(f'--embed-dir={INSTALLED_LIB.parent}')


def hook_recipe(recipe):
  recipe.add_step(
    FetchBazelisk,
    outputs=[BAZELISK],
    inputs=[__file__],
    desc='Downloading bazelisk',
    shortcut='get bazelisk')
  recipe.add_step(
    BazelBuild,
    outputs=BUILT,
    inputs=[BAZELISK, *hook.beam, *BAZEL_EXTRA_INPUTS, __file__],
    desc='Building TensorFlow (bazel; takes a long time)',
    shortcut='build tensorflow')
  # Install runs llvm-strip on Linux, so it waits for the LLVM install.
  recipe.add_step(
    Install,
    outputs=INSTALLED,
    inputs=[*BUILT, *llvm.hook.beam, __file__],
    desc='Installing TensorFlow',
    shortcut='install TensorFlow')
  # Sources that include TensorFlow headers wait for the install; by then the
  # bazel-fetched header repos that IncludeArgs points at exist as well.
  hook.beam = INSTALLED


hook = extension_helper.ExtensionHelper('TensorFlow', globals())
hook.FetchFromGit('https://github.com/tensorflow/tensorflow.git', f'v{VERSION}')
hook.SkipConfigure()
hook.InstallWhenIncluded(r'tensorflow/(cc|core)/')
hook.AddCompileArg(IncludeArgs)
# TensorFlow's headers use enum arithmetic that -std=gnu++26 rejects, so the
# translation unit that includes them compiles as gnu++17 - the standard
# TensorFlow itself builds with (appended after the global -std, so it wins).
hook.AddCompileArg('-std=gnu++17')
if platform == 'win32':
  hook.AddCompileArgs('-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-DNOGDI')
  hook.AddLinkArgs(str(INSTALLED_IMPLIB), '-Wl,/delayload:tensorflow.dll',
                   '-Wl,/defaultlib:delayimp.lib')
# On Linux automat links no TensorFlow library at all; the trampolines stand in
# for it (tensorflow_trampolines.cpp).
