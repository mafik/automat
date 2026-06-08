'''This module ensures that all of the necessary Debian dependencies are installed.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import os, shutil
from subprocess import run
from sys import platform

path_to_package = {
    "/usr/bin/python": "python-is-python3",
    "/usr/bin/clang": "clang",
    "/usr/bin/ld.lld": "lld",
    "/usr/bin/ninja": "ninja-build",
    "/usr/bin/cmake": "cmake",
    "/usr/include/zlib.h": "zlib1g-dev",
    "/usr/include/openssl/ssl.h": "libssl-dev",
    "/usr/include/gmock": "libgmock-dev",
    "/usr/bin/meson": "meson",
    "/usr/include/X11/Xlib.h": "libx11-dev",
    "/usr/include/X11/extensions/Xrandr.h": "libxrandr-dev",
    "/usr/include/X11/extensions/Xinerama.h": "libxinerama-dev",
    "/usr/include/X11/Xcursor/Xcursor.h": "libxcursor-dev",
    "/usr/include/X11/extensions/XInput2.h": "libxi-dev",
    "/usr/include/GL/gl.h": "libgl-dev",
    "/usr/include/fontconfig/fontconfig.h": "libfontconfig-dev",
    "/usr/bin/gperf": "gperf",
    "/usr/bin/bison": "bison",  # xkbcommon meson keymap parser (YACC)
    "/usr/include/sys/capability.h": "libcap-dev",
    "/usr/lib/python3/dist-packages/jinja2/__init__.py": "python3-jinja2"  # libsystemd meson codegen
}


def check_and_install():
    if platform != 'linux':
        return
    if shutil.which('apt-get') == None:
        # We don't auto-install system dependencies on non-Debian systems (yet)
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
        try:
            input()
        except EOFError:
            print("Got EOF - batch job detected. Continuing with the installation.")
        command = ["apt-get", "-y", "install"] + list(missing_packages)
        if os.geteuid() != 0:  # non-root users need `sudo` to install stuff
            command = ["sudo"] + command
        print(" ".join(command) + "\n")
        run(command, check=True)
