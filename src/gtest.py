# import fs_utils
# import cc
# import functools
# from cmake import CMakeArgs
# from make import Popen

# GOOGLETEST_SRC = fs_utils.project_root / 'vendor' / 'googletest-1.13.0'
# GOOGLETEST_OUT = fs_utils.build_dir / 'googletest'
# GMOCK_LIB = GOOGLETEST_OUT / 'lib' / 'gmock.lib'
# GTEST_LIB = GOOGLETEST_OUT / 'lib' / 'gtest.lib'
# GTEST_MAIN_LIB = GOOGLETEST_OUT / 'lib' / 'gtest_main.lib'
# cc.CXXFLAGS += ['-I', GOOGLETEST_SRC / 'googlemock' / 'include']
# cc.CXXFLAGS += ['-I', GOOGLETEST_SRC / 'googletest' / 'include']
# cc.TEST_DEPS += [GMOCK_LIB, GTEST_LIB, GTEST_MAIN_LIB]
# cc.TEST_LDFLAGS += ['-L', GOOGLETEST_OUT /
#                     'lib', '-lgmock', '-lgtest', '-lgtest_main']
# cc.TEST_ARGS += ['--gtest_color=yes']

# def AddSteps(recipe):
#     recipe.add_step(
#         functools.partial(Popen, CMakeArgs(False, True) +
#                           ['-S', GOOGLETEST_SRC, '-B', GOOGLETEST_OUT]),
#         outputs=[GOOGLETEST_OUT / 'build.ninja'],
#         inputs=[GOOGLETEST_SRC / 'CMakeLists.txt'],
#         name='Configure GoogleTest')
#     recipe.add_step(
#         functools.partial(Popen, ['ninja', '-C', str(GOOGLETEST_OUT)]),
#         outputs=[GMOCK_LIB, GTEST_LIB, GTEST_MAIN_LIB],
#         inputs=[GOOGLETEST_OUT / 'build.ninja'],
#         name='Build GoogleTest')
#     recipe.generated.add(GOOGLETEST_OUT)

from subprocess import run


def hook_final(srcs, objs, bins, recipe):
    # Shortcut recipe for running all tests (default build type)
    tests = [
        bin for bin in bins if '-lgtest' in bin.link_args and bin.build_type == '' and bin.path.stem != 'gtest']

    def run_tests():
        for test in tests:
            print(f'Running {test.path}')
            run([str(test.path)] + test.run_args, check=True)

    recipe.add_step(run_tests, outputs=[], inputs=tests,
                    desc='Running tests', shortcut='tests')
