'''Contains the graph of recipes to build Automaton.'''

from asyncio import subprocess
import args
import cc
import make
import fs_utils
import os
import functools
import re

from make import Popen
from subprocess import run
from pathlib import Path

CXXFLAGS = '-std=c++2b -Wno-c99-designator -DAUTOMATON -Ivendor -fcolor-diagnostics'.split()
LDFLAGS = '-flto -lpthread -lz -lssl -lcrypto -lfmt'.split()

# libc++ cannot be linked with gtest so we use libstdc++ instead.
CXXFLAGS += ['-stdlib=libstdc++']

# As of 2023-02 the compiler is a development version of clang++-17, installed from https://apt.llvm.org/.
# Once C++20 modules stabilize it should be ok to switch to default compiler.

CLANG_VERSION = '17'
CXX = os.environ['CXX'] = f'clang++-{CLANG_VERSION}'
CC = os.environ['CC'] = f'clang-{CLANG_VERSION}'
CXXFILT = f'llvm-cxxfilt-{CLANG_VERSION}'

if args.release:
    CXXFLAGS += ['-O3', '-DNDEBUG']

if args.debug:
    CXXFLAGS += ['-O0', '-g3', '-fretain-comments-from-system-headers']
    LDFLAGS += ['-rdynamic']

recipe = make.Recipe()

##################################
# Recipes for object files
###############################

PCM_DIR = fs_utils.project_tmp_dir / 'pcm'
OBJ_DIR = fs_utils.project_tmp_dir / 'obj'
CXXFLAGS += ['-fprebuilt-module-path=' + str(PCM_DIR)]

PCM_DIR.mkdir(parents=True, exist_ok=True)
OBJ_DIR.mkdir(parents=True, exist_ok=True)

def cxxfilt(line):
    cxx_identifier_re = "(_Z[a-zA-Z0-9_]+)"
    while match := re.search(cxx_identifier_re, line):
        cxx_identifier = match.group(1)
        try:
            demangled = run([CXXFILT, cxx_identifier], stdout=subprocess.PIPE, check=True).stdout.decode('utf-8').strip()
            # add ANSI bold escape sequence
            demangled = f'\033[90m{demangled}\033[0m'
            line = line.replace(cxx_identifier, demangled)
        except:
            break
    line = line.replace('std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>>', 'string')
    line = line.replace('std::basic_string_view<char, std::char_traits<char>>', 'string_view')
    line = line.replace('automaton::', '')
    line = line.replace('/usr/bin/ld: ', '')
    line = line.replace(str(OBJ_DIR), '')
    # remove module names
    while match := re.search('@[a-zA-Z0-9_]+', line):
        line = line.replace(match.group(0), '')
    # remove repetitions of type names like 'X[X]'
    line = re.sub(r'(.+)\[(\1)\]', r'\1', line)
    # remove offsets like '[filename:](.text[.symbol]+0x123):'
    line = re.sub(r'(.*:)?\(\.text(.+)?\+0x[0-9a-f]+\):', ' ', line)
    return line

def redirect_path(path):
    name = Path(path).name
    if path.endswith('.o'):
        return str(OBJ_DIR / name)
    elif cc.types[path] in ('precompiled module', 'system header unit', 'user header unit'):
        return str(PCM_DIR / name)
    elif cc.types[path] in ('test', 'main'):
        return name
    else:
        return path

types = dict()
graph = dict()
for path, deps in cc.graph.items():
    graph[redirect_path(path)] = [redirect_path(d) for d in deps]

# Workarounds for poor support of C++20 modules in clang.
# Without those dependencies clang will imagine ODR violations and fail to link.
# Feel free to remove if it doesn't break the build.
graph[redirect_path('gtest/gtest.h.pcm')].append(redirect_path('compare.pcm'))
graph[redirect_path('fmt/format.h.pcm')].append(redirect_path('memory.pcm'))
graph[redirect_path('gtest/gtest.h.pcm')].append(redirect_path('memory.pcm'))
graph[redirect_path('gmock/gmock.h.pcm')].append(redirect_path('memory.pcm'))
graph[redirect_path('gmock/gmock-more-matchers.h.pcm')].append(redirect_path('memory.pcm'))
graph[redirect_path('regex.pcm')].append(redirect_path('memory.pcm'))

header_units = dict()
for path, t in cc.types.items():
    real_path = redirect_path(path)
    types[real_path] = t
    if t in ('user header unit', 'system header unit'):
        header_units[real_path] = str(Path(path).with_suffix('')) # remove .pcm extension

tests = []

for path, deps in graph.items():
    t = types[path]
    args = [CXX] + CXXFLAGS
    if t in ('module interface', 'module implementation', 'test source', 'main source'):
        pass
    elif t == 'precompiled module':
        recipe.generated.add(path)
        ixx = [d for d in deps if d.endswith('.ixx')][0]
        assert ixx
        args += ['-fmodule-file=' + d for d in deps if d.endswith('.pcm')]
        args += ['-x', 'c++-module', ixx, '--precompile', '-o', path]
        builder = functools.partial(Popen, args)
        recipe.add_step(builder, outputs=[path], inputs=deps, name=Path(path).name)
    elif t == 'system header unit':
        recipe.generated.add(path)
        args += ['-fmodule-file=' + d for d in deps if d.endswith('.pcm')]
        args += ['-xc++-system-header', header_units[path], '--precompile', '-o', path]
        builder = functools.partial(Popen, args)
        recipe.add_step(builder, outputs=[path], inputs=deps, name=Path(path).name)
    elif t == 'user header unit':
        recipe.generated.add(path)
        args += ['-fmodule-file=' + d for d in deps if d.endswith('.pcm')]
        args += ['-xc++-user-header', header_units[path], '--precompile', '-o', path]
        builder = functools.partial(Popen, args)
        recipe.add_step(builder, outputs=[path], inputs=deps, name=Path(path).name)
    elif t == 'object file':
        recipe.generated.add(path)
        # replace last _ with . to get the original file name
        orig_name = re.sub(r'_(?=[^_]+$)', '.', Path(path).stem)
        args += [d for d in deps if d.endswith(orig_name)]
        args += ['-fmodule-file=' + d for d in deps if d.endswith('.pcm') and not d.endswith(orig_name)]
        args += ['-c', '-o', path]
        builder = functools.partial(Popen, args)
        recipe.add_step(builder, outputs=[path], inputs=deps, name=Path(path).name)
    elif t == 'test':
        args += ['-DGTEST_HAS_PTHREAD=1'] + deps + ['-o', path] + LDFLAGS + ['-lgtest_main', '-lgtest', '-lgmock']
        builder = functools.partial(Popen, args)
        recipe.add_step(builder, outputs=[path], inputs=deps, name=f'link {path}', stderr_prettifier=cxxfilt)
        runner = functools.partial(Popen, [f'./{path}', '--gtest_color=yes'])
        recipe.add_step(runner, outputs=[], inputs=[path], name=path)
        tests.append(path)
    elif t == 'main':
        args += deps + ['-o', path] + LDFLAGS
        builder = functools.partial(Popen, args)
        recipe.add_step(builder, outputs=[path], inputs=deps, name=f'link {path}', stderr_prettifier=cxxfilt)
    else:
        print(f"File '{path}' has unknown type '{types[path]}'. Dependencies:")
        for dep in deps:
            print(f'    {dep}')
        assert False

if tests:
    # run all tests sequentially
    def run_tests():
        for test in tests:
            run([f'./{test}', '--gtest_color=yes'], check=True)
    recipe.add_step(run_tests, outputs=[], inputs=tests, name='tests')

##########################
# Recipe for Clang language server
##########################

def compile_commands():
    print('Generating compile_commands.json...')
    with open('compile_commands.json', 'w') as f:
        print('[{', file=f)
        print(f'  "directory": "{ fs_utils.project_root }",', file=f)
        print(f'  "file": "src/base.cc",', file=f)
        print(f'  "output": "{OBJ_DIR / "base_cc.o"}",', file=f)
        print(f'  "arguments": [', file=f)
        args = [CXX] + CXXFLAGS
        for path, t in types.items():
            if t in ('system header unit', 'user header unit'):
                args += ['-fmodule-file=' + path]
        joined_args = ',\n    '.join(f'"{arg}"' for arg in args)
        print(f'    {joined_args}', file=f)
        print(f'  ]', file=f)
        print('}]', file=f)

recipe.add_step(compile_commands, ['compile_commands.json'], [])

##########################
# Recipe for server
#########################

def serve():
    return Popen(['./main'])

recipe.add_step(serve, [], ['main'], keep_alive = True)

if False:
    for step in recipe.steps:
        print('Step', step.name)
        print('  Inputs:')
        for inp in sorted(str(x) for x in step.inputs):
            print('    ', inp)
        print('  Outputs: ', step.outputs)
