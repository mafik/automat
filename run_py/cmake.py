import build

build.fast.CMAKE_BUILD_TYPE = 'RelWithDebInfo'
build.release.CMAKE_BUILD_TYPE = 'Release'
build.debug.CMAKE_BUILD_TYPE = 'Debug'

# CMake needs this policy to be enabled to respect `CMAKE_MSVC_RUNTIME_LIBRARY`
# https://cmake.org/cmake/help/latest/policy/CMP0091.html
# When vk-bootstrap sets minimum CMake version >= 3.15, the policy define can be removed.
build.fast.CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreaded'
build.release.CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreaded'
build.debug.CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreadedDebug'

def CMakeArgs(build_type: build.BuildType):
    CMAKE_BUILD_TYPE = build_type.CMAKE_BUILD_TYPE
    CMAKE_MSVC_RUNTIME_LIBRARY = build_type.CMAKE_MSVC_RUNTIME_LIBRARY

    cmake_args = ['cmake', '-G', 'Ninja', f'-D{CMAKE_BUILD_TYPE=}',
                  f'-DCMAKE_C_COMPILER={build.compiler_c}', f'-DCMAKE_CXX_COMPILER={build.compiler}']

    cmake_args += ['-DCMAKE_POLICY_DEFAULT_CMP0091=NEW', f'-D{CMAKE_MSVC_RUNTIME_LIBRARY=}']

    return cmake_args
