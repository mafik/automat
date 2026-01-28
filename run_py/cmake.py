# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
import build
import ninja

if build.fast:
  CMAKE_BUILD_TYPE = 'RelWithDebInfo'
  CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreaded'
elif build.release:
  CMAKE_BUILD_TYPE = 'Release'
  CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreaded'
elif build.debug:
  CMAKE_BUILD_TYPE = 'Debug'
  CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreadedDebug'


def CMakeArgs(extra_defines: dict[str, str] = {}):
    CMAKE_MAKE_PROGRAM = str(ninja.BIN)
    CMAKE_INSTALL_LIBDIR = 'lib64'

    # CMAKE_POLICY_DEFAULT_CMP0091=NEW tells CMake to override the CRT selection of individual targets
    # (some targets, like Leptonica don't respect CMAKE_MSVC_RUNTIME_LIBRARY) and just use whatever we
    # set.
    cmake_args = ['cmake', '-G', 'Ninja', f'-D{CMAKE_BUILD_TYPE=}', f'-D{CMAKE_MAKE_PROGRAM=}',
                  f'-DCMAKE_C_COMPILER={build.compiler_c}', f'-DCMAKE_CXX_COMPILER={build.compiler}',
                  f'-D{CMAKE_MSVC_RUNTIME_LIBRARY=}', f'-D{CMAKE_INSTALL_LIBDIR=}',
                  '-DCMAKE_POLICY_DEFAULT_CMP0091=NEW']

    cmake_args += ['-DCMAKE_INSTALL_PREFIX=' + str(build.PREFIX)]

    for name, value in extra_defines.items():
      cmake_args += ['-D' + name + '=' + value]

    return cmake_args
