'''Module which keeps track of all C++ sources.'''

from . import fs_utils
import collections
import itertools
import re
import string
from . import args
from . import clang
from sys import platform
from pathlib import Path

defines = set()
defines.add(fs_utils.project_name.upper())
if platform == 'win32':
    defines.add('NOMINMAX')
    # Prefer UTF-8 over UTF-16. This means no "UNICODE" define.
    # https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page
    # DO NOT ADD: defines.add('UNICODE')
    # <windows.h> has a side effect of defining ERROR macro.
    # Adding NOGDI prevents it from happening.
    defines.add('NOGDI')
    # MSVCRT <source_location> needs __cpp_consteval.
    # As of Clang 16 it's not defined by default.
    # If future Clangs add it, the manual definition can be removed.
    defines.add('__cpp_consteval')
    # Silence some MSCRT-specific deprecation warnings.
    defines.add('_CRT_SECURE_NO_WARNINGS')
    # No clue what it precisely does but many projects use it.
    defines.add('WIN32_LEAN_AND_MEAN')
    defines.add('VK_USE_PLATFORM_WIN32_KHR')
    # Set Windows version to Windows 10.
    defines.add('_WIN32_WINNT=0x0A00')
    defines.add('WINVER=0x0A00')
elif platform == 'linux':
    defines.add('VK_USE_PLATFORM_XCB_KHR')

defines.add('SK_GANESH')
defines.add('SK_VULKAN')
defines.add('SK_USE_VMA')
defines.add('SK_SHAPER_HARFBUZZ_AVAILABLE')

if args.debug:
    defines.add('_DEBUG')
    defines.add('SK_DEBUG')
    # This subtly affects the Skia ABI and leads to crashes when passing sk_sp across the library boundary.
    # For more interesting defines, check out:
    # https://github.com/google/skia/blob/main/include/config/SkUserConfig.h
    defines.add('SK_TRIVIAL_ABI=[[clang::trivial_abi]]')
else:
    defines.add('NDEBUG')


graph = collections.defaultdict(set)
types = dict()


def reset():
    graph.clear()
    types.clear()


binary_extension = '.exe' if platform == 'win32' else ''


def add_translation_unit(path):
    path = Path(path)  # because backslashes on Windows
    path_o = path.with_name(path.stem + '.o')
    types[str(path)] = 'translation unit'
    types[str(path_o)] = 'object file'
    depends(path_o, on=path)


def add_object(path):
    path = Path(path)
    types[str(path)] = 'object file'
    graph[str(path)]


def add_header(path):
    path = Path(path)
    types[str(path)] = 'header'

# path extension doesn't matter - can be .cc, .h, or even none at all


def add_bin(path, typ='main'):
    path = Path(path)
    path_bin = path.with_suffix(binary_extension)
    path_o = path.with_name(path.stem + '.o')
    types[str(path_bin)] = typ
    depends(path_bin, on=path_o)


def depends(what, on):
    graph[str(what)].add(str(on))


def scan(dir):
    srcs = []
    for ext in ['.cc', '.hh', '.h', '.c']:
        srcs.extend(dir.glob(f'**/*{ext}'))

    for path_abs in srcs:
        path = path_abs.relative_to(fs_utils.project_root)

        if path.suffix == '.cc' or path.suffix == '.c':
            add_translation_unit(path)
        elif path.suffix == '.h' or path.suffix == '.hh':
            add_header(path)

        if_stack = [True]
        current_defines = defines.copy() | clang.default_defines
        line_number = 0

        for line in open(path_abs, encoding='utf-8').readlines():
            line_number += 1

            # Minimal preprocessor. This allows us to skip platform-specific imports.

            # This regular experession captures most of #if defined/#ifdef variants in one go.
            # ?: at the beginning of a group means that it's non-capturing
            # ?P<...> ate the beginning of a group assigns it a name
            match = re.match(
                '^#(?P<el>el(?P<else>se)?)?(?P<end>end)?if(?P<neg1>n)?(?:def)? (?P<neg2>!)?(?:defined)?(?:\()?(?P<id>[a-zA-Z0-9_]+)(?:\))?', line)
            if match:
                test = match.group('id') in current_defines
                if match.group('neg1') or match.group('neg2'):
                    test = not test
                if match.group('else'):
                    test = not if_stack[-1]

                if match.group('end'):  # endif
                    if_stack.pop()
                elif match.group('el'):  # elif
                    if_stack[-1] = test
                else:  # if
                    if_stack.append(test)
                continue

            if not if_stack[-1]:
                continue

            # Actual scanning starts here

            match = re.match('^#include \"([a-zA-Z0-9_/\.-]+\.hh?)\"', line)
            if match:
                include = Path(match.group(1))
                dep = path.parent / include  # try relative to current source file
                if not dep.exists():
                    dep = include  # try relative to project root
                types[str(dep)] = 'header'
                depends(path, on=dep)

            match = re.match('^int main\(', line)
            if match:
                add_bin(path, 'main')

            match = re.match('^TEST(_F)?\\(', line)
            if match:
                add_bin(path, 'test')


def propagate_deps():
    binaries = [p for p, t in types.items() if t in ('test', 'main')]

    # Link object files for each header included in binaries.
    for path in binaries:
        deps = list(graph[path])
        visited = set()
        while len(deps) > 0:
            dep = deps.pop()
            if dep in visited:
                continue
            visited.add(dep)
            if types[dep] == 'header':
                object_file = dep.rsplit('.', 1)[0] + '.o'
                if object_file in graph:
                    depends(path, on=object_file)
                    deps.append(object_file)
            deps.extend(graph[dep])

    objects = [p for p, t in types.items() if t == 'object file']

    # Rebuild objects whenever any of the included header changes.
    for path in objects:
        deps = list(graph[path])
        visited = set()
        while len(deps) > 0:
            dep = deps.pop()
            if dep in visited:
                continue
            visited.add(dep)
            if types[dep] == 'header':
                depends(path, on=dep)
            deps.extend(graph[dep])


def print_debug():
    print('C++ dependency graph')
    for path in sorted(types.keys()):
        print(f' "{path}" : {types[path]}')
        for dep in sorted(graph[path]):
            print(f'  - "{dep}" : {types[dep]}')


byte_to_c_string_table = {c: chr(c) for c in range(32, 127)}
byte_to_c_string_table[0x00] = '\\0'
byte_to_c_string_table[0x22] = '\\"'
byte_to_c_string_table[0x5c] = '\\\\'
byte_to_c_string_table[0x07] = '\\a'
byte_to_c_string_table[0x08] = '\\b'
byte_to_c_string_table[0x0c] = '\\f'
byte_to_c_string_table[0x0a] = '\\n'
byte_to_c_string_table[0x0d] = '\\r'
byte_to_c_string_table[0x09] = '\\t'
byte_to_c_string_table[0x0b] = '\\v'

digit_bytes = set(ord(c) for c in string.digits)


def byte_to_c_string(b, next_b=None):
    if b in byte_to_c_string_table:
        return byte_to_c_string_table[b]
    elif next_b in digit_bytes:
        return '\\' + format(b, '03o')
    else:
        return '\\' + format(b, 'o')


def c_string_from_bytes(bytes):
    # x is a dummy byte
    return '"' + ''.join(byte_to_c_string(b, next_b) for b, next_b in itertools.pairwise(bytes + b'x')) + '"'
