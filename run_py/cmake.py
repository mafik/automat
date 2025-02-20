# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
import build
import ninja

build.fast.CMAKE_BUILD_TYPE = 'RelWithDebInfo'
build.release.CMAKE_BUILD_TYPE = 'Release'
build.debug.CMAKE_BUILD_TYPE = 'Debug'

build.fast.CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreaded'
build.release.CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreaded'
build.debug.CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreadedDebug'

def CMakeArgs(build_type: build.BuildType, extra_defines: dict[str, str] = {}):
    CMAKE_BUILD_TYPE = build_type.CMAKE_BUILD_TYPE
    CMAKE_MSVC_RUNTIME_LIBRARY = build_type.CMAKE_MSVC_RUNTIME_LIBRARY
    CMAKE_MAKE_PROGRAM = str(ninja.BIN)

    cmake_args = ['cmake', '-G', 'Ninja', f'-D{CMAKE_BUILD_TYPE=}', f'-D{CMAKE_MAKE_PROGRAM=}',
                  f'-DCMAKE_C_COMPILER={build.compiler_c}', f'-DCMAKE_CXX_COMPILER={build.compiler}',
                  f'-D{CMAKE_MSVC_RUNTIME_LIBRARY=}']

    cmake_args += ['-DCMAKE_INSTALL_PREFIX=' + str(build_type.PREFIX())]

    for name, value in extra_defines.items():
      cmake_args += ['-D' + name + '=' + value]

    return cmake_args
