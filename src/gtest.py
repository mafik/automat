# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
import build
from extension_helper import ExtensionHelper

hook = ExtensionHelper('googletest', globals())
hook.FetchFromURL('https://github.com/google/googletest/releases/download/v1.16.0/googletest-1.16.0.tar.gz')
hook.ConfigureOption('CMAKE_CXX_STANDARD', '20')
hook.ConfigureWithCMake('{PREFIX}/include/gtest/gtest.h')
hook.InstallWhenIncluded(r'(gmock/gmock.h|gtest/gtest.h)')

# Shortcut recipe for running all tests (default build type)
# def hook_final(srcs, objs, bins, recipe):
#   tests = [
#       bin for bin in bins if '-lgtest' in bin.link_args and bin.build_type == build.fast and bin.path.stem != 'gtest']

#   def run_tests():
#     for test in tests:
#         print(f'Running {test.path}')
#         run([str(test.path)] + test.run_args, check=True)

#   recipe.add_step(run_tests, outputs=[], inputs=tests,
#                   desc='Running tests', shortcut='tests')
