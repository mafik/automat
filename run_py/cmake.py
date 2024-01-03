import build

def CMakeArgs(build_type):
    if build_type == '':
        CMAKE_BUILD_TYPE = 'RelWithDebInfo'
    elif build_type == 'release':
        CMAKE_BUILD_TYPE = 'Release'
    elif build_type == 'debug':
        CMAKE_BUILD_TYPE = 'Debug'
    else:
        raise ValueError(f'Unknown build type: "{build_type}"')

    cmake_args = ['cmake', '-G', 'Ninja', f'-D{CMAKE_BUILD_TYPE=}',
                  f'-DCMAKE_C_COMPILER={build.compiler_c}', f'-DCMAKE_CXX_COMPILER={build.compiler}']

    # CMake needs this policy to be enabled to respect `CMAKE_MSVC_RUNTIME_LIBRARY`
    # https://cmake.org/cmake/help/latest/policy/CMP0091.html
    # When vk-bootstrap sets minimum CMake version >= 3.15, the policy define can be removed.
    CMAKE_MSVC_RUNTIME_LIBRARY = 'MultiThreadedDebug' if build_type == 'debug' else 'MultiThreaded'
    cmake_args += ['-DCMAKE_POLICY_DEFAULT_CMP0091=NEW',
                   f'-D{CMAKE_MSVC_RUNTIME_LIBRARY=}']

    return cmake_args
