# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

# Static TensorFlow. Upstream stopped shipping the libtensorflow C library
# and offers no static target at all, so this extension builds the C API
# with bazel and folds it into one archive:
#
#  1. A patched-in cc_binary (tensorflow/c/automat/BUILD) links the whole
#     C API and its static dependencies into a single relocatable object
#     ("ld -r"). Empty stub archives shadow glibc/libstdc++ so the C runtime
#     stays external; lld's --undefined-glob pulls every TF_*/TFE_* symbol
#     that plain archive linking would skip; --no-gc-sections overrides the
#     toolchain flag that would strip a relocatable link empty.
#  2. llvm-objcopy localizes everything except TF_*/TFE_*, so the bundled
#     LLVM, protobuf and absl can never collide with Automat's own copies
#     (nor leak into the dynamic symbol table - the lavapipe rule).
#  3. ar wraps the object as PREFIX/lib64/libtensorflow.a and the C API
#     headers land in PREFIX/include.
#
# bazel scratch goes to build/bazel_root (shared across variants; the
# product is variant-independent "-c opt"). The install base stays under
# the user's home because bazel fsyncs during extraction, which network
# filesystems may reject.

import os
import subprocess
from pathlib import Path

import build
import extension_helper
import fs_utils
import make

BAZELISK_URL = 'https://github.com/bazelbuild/bazelisk/releases/download/v1.27.0/bazelisk-linux-amd64'

CHECKOUT = fs_utils.third_party_dir / 'TensorFlow'
BAZEL_ROOT = fs_utils.build_dir / 'bazel_root'
BAZELISK = BAZEL_ROOT / 'bazelisk'
MERGED = CHECKOUT / 'bazel-bin' / 'tensorflow' / 'c' / 'automat' / 'automat_tf_merged'

BAZEL_FLAGS = [
  '-c', 'opt',
  '--config=monolithic',
  '--force_pic',
  '--spawn_strategy=local',
  # clang-20 module layering rejects grpc's zconf.h use.
  '--per_file_copt=external/com_github_grpc_grpc/.*@-Wno-private-header',
]

# The C API headers plus their transitive includes, looked up in the checkout
# first and then in the bazel-fetched dependency repos.
HEADER_GLOBS = [
  'tensorflow/c/*.h',
  'tensorflow/c/eager/*.h',
  'tensorflow/core/platform/ctstring*.h',
  'xla/tsl/c/*.h',
  'xla/tsl/platform/ctstring*.h',
  'tsl/platform/ctstring*.h',
]


def HeaderRoots():
  external = BAZEL_ROOT.glob('*/external')
  roots = [CHECKOUT]
  for ext in external:
    roots += [ext / 'local_xla', ext / 'local_tsl']
  return roots


def FetchBazelisk():
  BAZEL_ROOT.mkdir(parents=True, exist_ok=True)
  extension_helper.download_from_url(BAZELISK_URL, BAZELISK)
  BAZELISK.chmod(0o755)


def BazelBuild():
  env = os.environ.copy()
  env['CC'] = build.compiler_c
  env['CXX'] = build.compiler
  return make.Popen(
    [BAZELISK, f'--install_base={Path.home() / ".cache" / "automat_bazel_install"}',
     f'--output_user_root={BAZEL_ROOT}', 'build'] + BAZEL_FLAGS +
    ['//tensorflow/c/automat:automat_tf_merged'],
    env=env, cwd=CHECKOUT)


def MergeArchive():
  lib_dir = build.PREFIX / 'lib64'
  lib_dir.mkdir(parents=True, exist_ok=True)
  localized = BAZEL_ROOT / 'tf_localized.o'
  objcopy = build.PREFIX / 'bin' / 'llvm-objcopy'
  subprocess.run([objcopy, '-w', '--keep-global-symbol=TF_*', '--keep-global-symbol=TFE_*',
                  MERGED, localized], check=True)
  archive = lib_dir / 'libtensorflow.a'
  archive.unlink(missing_ok=True)
  subprocess.run(['ar', 'rcs', archive, localized], check=True)
  localized.unlink()


def InstallHeaders():
  include = build.PREFIX / 'include'
  for pattern in HEADER_GLOBS:
    found = False
    for root in HeaderRoots():
      for src in root.glob(pattern):
        dst = include / src.relative_to(root)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_bytes(src.read_bytes())
        found = True
    if not found:
      raise RuntimeError(f'TensorFlow headers not found: {pattern}')


def hook_recipe(recipe):
  recipe.add_step(
    FetchBazelisk,
    outputs=[BAZELISK],
    inputs=[__file__],
    desc='Downloading bazelisk',
    shortcut='get bazelisk')
  recipe.add_step(
    BazelBuild,
    outputs=[MERGED],
    inputs=[BAZELISK, hook.checkout_dir / 'patch0.marker', __file__],
    desc='Building TensorFlow (bazel; takes a long time)',
    shortcut='build tensorflow')
  recipe.add_step(
    MergeArchive,
    outputs=[build.PREFIX / 'lib64' / 'libtensorflow.a'],
    inputs=[MERGED, __file__],
    desc='Merging TensorFlow into libtensorflow.a',
    shortcut='merge tensorflow')
  recipe.add_step(
    InstallHeaders,
    outputs=[build.PREFIX / 'include' / 'tensorflow' / 'c' / 'c_api.h'],
    inputs=[MERGED, __file__],
    desc='Installing TensorFlow headers',
    shortcut='install tensorflow headers')


hook = extension_helper.ExtensionHelper('TensorFlow', globals())
hook.FetchFromGit('https://github.com/tensorflow/tensorflow.git', 'v2.20.0')
hook.SkipConfigure()


# An empty ar archive is just its magic line.
EMPTY_ARCHIVE = b'!<arch>\n'

MERGED_BUILD = '''# One relocatable object holding the whole TF C API with its static
# dependencies; Automat turns it into libtensorflow.a (see src/tensorflow.py).
# The undefined-globs force every TF_*/TFE_* definition out of the dep
# archives, which plain archive linking would otherwise skip.
cc_binary(
    name = "automat_tf_merged",
    linkopts = [
        # Empty stubs shadow the glibc/libstdc++ static archives that the
        # dep linkopts (-lm, -lstdc++, ...) would otherwise merge into the
        # relocatable object; the C runtime stays external and resolves in
        # the final Automat link.
        "-Ltensorflow/c/automat/stubs",
        "-r",
        "-nostdlib",
        "-no-pie",
        "-fuse-ld=lld",
        "-Wl,--no-gc-sections",
        "-Wl,--undefined-glob=TF_*",
        "-Wl,--undefined-glob=TFE_*",
    ],
    linkstatic = True,
    deps = [
        "//tensorflow/c:c_api",
        "//tensorflow/c/eager:c_api",
    ],
)
'''


def _patch_merged_target(marker):
  pkg = hook.src_dir / 'tensorflow' / 'c' / 'automat'
  stubs = pkg / 'stubs'
  stubs.mkdir(parents=True, exist_ok=True)
  (pkg / 'BUILD').write_text(MERGED_BUILD)
  for lib in ('m', 'pthread', 'dl', 'rt', 'stdc++'):
    (stubs / f'lib{lib}.a').write_bytes(EMPTY_ARCHIVE)
  marker.touch()


hook.PatchSources(_patch_merged_target)
hook.InstallWhenIncluded(r'tensorflow/c/')
hook.AddLinkArgs('-ltensorflow')
