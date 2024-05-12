'''This module ensures that all of the necessary Debian dependencies are installed.'''

import os
from subprocess import run
from sys import platform

path_to_package = {
    "/usr/include/zlib.h": "zlib1g-dev",
    "/usr/include/openssl/ssl.h": "libssl-dev",
    "/usr/bin/inotifywait": "inotify-tools",
    "/usr/include/gmock": "libgmock-dev",
}


def check_and_install():
    if platform != 'linux':
        return
    missing_packages = set()
    for path, package in path_to_package.items():
        if not os.path.exists(path):
            missing_packages.add(package)
    if missing_packages:
        print("Some packages are missing from your system. Will try to install them automatically:\n")
        print("  ", ', '.join(missing_packages))
        print("In case of errors with clang or libc++ installation - add the repositories from https://apt.llvm.org/ and re-run this script.\n")
        print("Press enter to continue or Ctrl+C to cancel.")
        input()
        command = ["apt", "-y", "install"] + list(missing_packages)
        if os.geteuid() != 0:  # non-root users need `sudo` to install stuff
            command = ["sudo"] + command
        print(" ".join(command) + "\n")
        run(command, check=True)
