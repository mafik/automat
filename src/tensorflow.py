# SPDX-FileCopyrightText: Copyright 2026 Automat Authors
# SPDX-License-Identifier: MIT

# Warning: coded with a stochastic parrot

# Static TensorFlow C++ API. Upstream ships no static library and no static
# target, so this extension builds the C++ API with bazel and folds it into one
# archive that Automat links directly - there is no C wrapper:
#
#  1. A patched-in cc_binary (tensorflow/automat/BUILD) does a relocatable link
#     ("ld -r") of the C++ graph API and the op kernels into one object.
#     root.cc names the API entry points so the relocatable link retains them.
#     all_kernels, the op registry and the direct-session factory are alwayslink,
#     so every kernel, every op definition and the session factory come along -
#     the C++ Session path needs all three registered at process start.
#     Empty stub archives shadow glibc/libstdc++ so the C runtime stays external
#     and resolves in the final Automat link.
#  2. llvm-objcopy keeps the C++ API namespaces global (tensorflow, tsl,
#     protobuf, absl, Eigen) and localizes everything else, so TF's bundled
#     LLVM and MLIR can never collide with Automat's own LLVM (nor leak into the
#     dynamic symbol table - the lavapipe rule). Automat keeps full control of
#     its own LLVM; this one stays hidden inside the archive.
#  3. ar wraps the single object as PREFIX/lib64/libtensorflow.a. Because the
#     archive holds one object, referencing any TF symbol pulls the whole thing,
#     so the kernel registrations survive into the binary.
#
# Automat compiles against the C++ headers in place - the tensorflow source
# tree, the generated headers under bazel-bin, and the absl/Eigen/protobuf/tsl/
# xla/ml_dtypes repos that bazel fetched - so there is no header copy step.
#
# bazel reuses the warm output root under the user's home (also where bazel
# fsyncs during extraction, which network filesystems may reject).

import os
import subprocess
from pathlib import Path

import build
import extension_helper
import fs_utils
import make
import src

llvm = src.load_extension('llvm')

BAZELISK_URL = 'https://github.com/bazelbuild/bazelisk/releases/download/v1.27.0/bazelisk-linux-amd64'

CHECKOUT = fs_utils.third_party_dir / 'TensorFlow'
BAZEL_ROOT = Path.home() / 'bazel_root'
BAZELISK = BAZEL_ROOT / 'bazelisk'
MERGED = CHECKOUT / 'bazel-bin' / 'tensorflow' / 'automat' / 'automat_tf'
ARCHIVE = build.PREFIX / 'lib64' / 'libtensorflow.a'

BAZEL_FLAGS = [
  '-c', 'opt',
  '--config=monolithic',
  '--force_pic',
  '--spawn_strategy=local',
  # clang-20 module layering rejects grpc's zconf.h use.
  '--per_file_copt=external/com_github_grpc_grpc/.*@-Wno-private-header',
]

# The C++ API's public headers live in these bazel-fetched repos, alongside the
# tensorflow source tree and its generated headers.
INCLUDE_REPOS = ['com_google_absl', 'eigen_archive', 'com_google_protobuf/src', 'local_tsl',
                 'local_xla', 'ml_dtypes_py']

# Namespaces of the C++ API that Automat links against. Everything outside them
# is localized so it cannot clash with Automat's own copies (notably LLVM). The
# keep patterns match the mangled namespace component (e.g. "3tsl"), not a bare
# substring, so an unrelated symbol like llvm's remove_leading_dotslash - whose
# name happens to contain "tsl" - is still localized.
KEEP_NAMESPACES = ['tensorflow', 'tsl', 'protobuf', 'absl', 'Eigen']


def FetchBazelisk():
  BAZEL_ROOT.mkdir(parents=True, exist_ok=True)
  extension_helper.download_from_url(BAZELISK_URL, BAZELISK)
  BAZELISK.chmod(0o755)


def BazelBuild():
  env = os.environ.copy()
  env['CC'] = build.compiler_c
  env['CXX'] = build.compiler
  return make.Popen(
    [BAZELISK, f'--output_user_root={BAZEL_ROOT}', 'build'] + BAZEL_FLAGS +
    ['//tensorflow/automat:automat_tf'],
    env=env, cwd=CHECKOUT)


def ExternalRepos():
  # bazel-bin resolves to <output_base>/execroot/org_tensorflow/bazel-out/k8-opt/bin.
  return (CHECKOUT / 'bazel-bin').resolve().parents[4] / 'external'


def IncludeArgs(*_):
  # Each dependency repo has two header roots: the fetched source under
  # <output_base>/external and bazel's generated output (the .pb.h protobuf
  # headers) under bazel-bin/external.
  source_external = ExternalRepos()
  bin_dir = (CHECKOUT / 'bazel-bin').resolve()
  gen_external = bin_dir / 'external'
  roots = [CHECKOUT, bin_dir]
  for repo in INCLUDE_REPOS:
    roots += [source_external / repo, gen_external / repo]
  return [f'-I{root}' for root in roots if root.exists()]


def MergeArchive():
  ARCHIVE.parent.mkdir(parents=True, exist_ok=True)
  localized = BAZEL_ROOT / 'tf_localized.o'
  objcopy = build.PREFIX / 'bin' / 'llvm-objcopy'
  keep = []
  for ns in KEEP_NAMESPACES:
    # Itanium mangling prefixes each namespace with its length: "3tsl", not "tsl".
    keep += ['--keep-global-symbol', f'*{len(ns)}{ns}*']
  subprocess.run([objcopy, '-w', *keep, MERGED, localized], check=True)
  ARCHIVE.unlink(missing_ok=True)
  subprocess.run(['ar', 'rcs', ARCHIVE, localized], check=True)
  localized.unlink()


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
    inputs=[BAZELISK, *hook.beam, __file__],
    desc='Building TensorFlow (bazel; takes a long time)',
    shortcut='build tensorflow')
  # MergeArchive runs llvm-objcopy, so it waits for the LLVM install.
  recipe.add_step(
    MergeArchive,
    outputs=[ARCHIVE],
    inputs=[MERGED, *llvm.hook.beam, __file__],
    desc='Installing TensorFlow',
    shortcut='install TensorFlow')
  # Sources that include TensorFlow headers wait for the archive; by then the
  # bazel-fetched header repos that IncludeArgs points at exist as well.
  hook.beam = [ARCHIVE]


hook = extension_helper.ExtensionHelper('TensorFlow', globals())
hook.FetchFromGit('https://github.com/tensorflow/tensorflow.git', 'v2.20.0')
hook.SkipConfigure()


# An empty ar archive is just its magic line.
EMPTY_ARCHIVE = b'!<arch>\n'

AUTOMAT_BUILD = '''# One relocatable object holding the TensorFlow C++ API and its op kernels;
# Automat turns it into libtensorflow.a (see src/tensorflow.py). root.cc names
# the API so the relocatable link retains it; the op registry, all_kernels and
# the direct-session factory are alwayslink, so their registrars come along.
cc_binary(
    name = "automat_tf",
    srcs = ["root.cc"],
    linkopts = [
        # Empty stubs shadow the glibc/libstdc++ static archives that the dep
        # linkopts (-lm, -lstdc++, ...) would otherwise merge in; the C runtime
        # stays external and resolves in the final Automat link.
        "-Ltensorflow/automat/stubs",
        "-r",
        "-nostdlib",
        "-no-pie",
        "-fuse-ld=lld",
        "-Wl,--no-gc-sections",
    ],
    linkstatic = True,
    deps = [
        "//tensorflow/cc:cc_ops",
        "//tensorflow/cc:client_session",
        "//tensorflow/core:framework",
        "//tensorflow/core:core_cpu",
        "//tensorflow/core:ops",
        "//tensorflow/core:all_kernels",
        "//tensorflow/core:direct_session",
    ],
)
'''

AUTOMAT_ROOT = '''// Warning: coded with a stochastic parrot
// References the TensorFlow C++ API so the relocatable ("-r") link in
// tensorflow/automat/BUILD retains it in libtensorflow.a (see src/tensorflow.py).
// This mirrors the surface src/library_tensorflow.cpp uses: Automat builds ops
// by runtime name through NodeBuilder, so only the graph, session and tensor
// machinery is named here - the op kernels come from all_kernels (alwayslink).
#include "tensorflow/cc/client/client_session.h"
#include "tensorflow/cc/framework/scope.h"
#include "tensorflow/cc/ops/array_ops.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/graph/node_builder.h"

extern "C" void automat_tf_link_root() {
  using namespace tensorflow;
  Scope scope = Scope::NewRootScope();
  auto input = ops::Placeholder(scope.WithOpName("input"), DT_FLOAT);
  Node* node = nullptr;
  (void)NodeBuilder(scope.GetUniqueNameForOp("op"), "Square")
      .Input(input.node())
      .Finalize(scope.graph(), &node);
  ClientSession session(scope);
  Tensor input_tensor(DT_FLOAT, TensorShape({1}));
  std::vector<Tensor> outputs;
  (void)session.Run({{input, input_tensor}}, {Output(node)}, &outputs);
}
'''


def _patch_automat_target(marker):
  pkg = hook.src_dir / 'tensorflow' / 'automat'
  stubs = pkg / 'stubs'
  stubs.mkdir(parents=True, exist_ok=True)
  (pkg / 'BUILD').write_text(AUTOMAT_BUILD)
  (pkg / 'root.cc').write_text(AUTOMAT_ROOT)
  for lib in ('m', 'pthread', 'dl', 'rt', 'stdc++'):
    (stubs / f'lib{lib}.a').write_bytes(EMPTY_ARCHIVE)
  marker.touch()


hook.PatchSources(_patch_automat_target)
hook.InstallWhenIncluded(r'tensorflow/(cc|core)/')
hook.AddCompileArg(IncludeArgs)
# TensorFlow's headers use enum arithmetic that -std=gnu++26 rejects, so the one
# translation unit that includes them (tensorflow_runtime.cpp) compiles as
# gnu++17 - the standard TensorFlow itself builds with. This overrides the
# global -std because it is appended after it.
hook.AddCompileArg('-std=gnu++17')
hook.AddLinkArgs('-ltensorflow')
