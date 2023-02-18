'''This module ensures that all of the necessary Debian dependencies are installed.'''

import os
from subprocess import run

if '/usr/lib/llvm-13/bin/' not in os.environ['PATH']:
  os.environ['PATH'] += os.pathsep + '/usr/lib/llvm-13/bin/'

path_to_package = {
    "/usr/bin/cmake": "cmake",
    "/usr/include/zlib.h": "zlib1g-dev",
    "/usr/include/openssl/ssl.h": "libssl-dev",
    "/usr/bin/7za": "p7zip-full",
    "/usr/bin/brotli": "brotli",
    "/usr/bin/clang++": "clang",
    #"/usr/bin/g++": "g++",
    #"/usr/share/doc/libc++abi-dev/": "libc++abi-dev",
    "/usr/bin/inotifywait": "inotify-tools",
    "/usr/include/benchmark/benchmark.h": "libbenchmark-dev",
    "/usr/include/fmt/format.h": "libfmt-dev",
    "/usr/include/gtest/gtest.h": "libgtest-dev",
    "/usr/include/gmock": "libgmock-dev"
}

def check_and_install():
  missing_packages = set()
  for path, package in path_to_package.items():
    if not os.path.exists(path):
      missing_packages.add(package)
  if missing_packages:
    print("Some packages are missing from your system. Will try to install them automatically:\n")
    print("In case of errors with clang or libc++ installation - add the repositories from https://apt.llvm.org/ and re-run this script.\n")
    input()
    command = ["apt", "-y", "install"] + list(missing_packages)
    if os.geteuid() != 0:  # non-root users need `sudo` to install stuff 
      command = ["sudo"] + command
    print(" ".join(command) + "\n")
    run(command, check=True)
