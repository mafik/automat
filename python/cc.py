'''Module which keeps track of all C++ sources.'''

import fs_utils
import collections
import re
import args
from sys import platform

defines = set()
defines.add(fs_utils.project_name.upper())
if platform == 'win32':
    defines.add('_WIN32')
if args.release:
    defines.add('NDEBUG')
if args.debug:
    defines.add('_DEBUG')

from sys import platform

srcs = []
for ext in ['.cc', '.h']:
    srcs.extend(fs_utils.project_root.glob(f'src/**/*{ext}'))

graph = collections.defaultdict(set)
types = dict()

binary_extension = '.exe' if platform == 'win32' else ''

def depends(what, on):
    graph[str(what)].add(str(on))

for path_abs in srcs:
    parent = path_abs.parent
    path = path_abs.relative_to(fs_utils.project_root)
    path_bin = path.with_suffix(binary_extension)

    if path.suffix == '.cc':
        path_o = path.with_name(path.stem + '.o')
        types[str(path)] = 'translation unit'
        types[str(path_o)] = 'object file'
        depends(path_o, on=path)
    elif path.suffix == '.h':
        path_o = None
        types[str(path)] = 'header'

    if_stack = [True]
    current_defines = defines.copy()
    line_number = 0

    for line in open(path_abs, encoding='utf-8').readlines():
        line_number += 1

        # Minimal preprocessor. This allows us to skip platform-specific imports.

        match = re.match('^#if defined\(([a-zA-Z0-9_]+)\)', line)
        if match:
            if_stack.append(match.group(1) in current_defines)
            continue

        match = re.match('^#if !defined\(([a-zA-Z0-9_]+)\)', line)
        if match:
            if_stack.append(match.group(1) not in current_defines)
            continue

        match = re.match('^#elif defined\(([a-zA-Z0-9_]+)\)', line)
        if match:
            if_stack[-1] = match.group(1) in current_defines
            continue
        
        match = re.match('^#else', line)
        if match:
            if_stack[-1] = not if_stack[-1]
            continue

        match = re.match('^#endif', line)
        if match:
            if_stack.pop()

        if not if_stack[-1]:
            continue

        # Actual scanning starts here
        
        match = re.match('^#include \"([a-zA-Z0-9_/\.-]+)\"', line)
        if match:
            dep = path.with_name(match.group(1))
            types[str(dep)] = 'header'
            depends(path, on=dep)

        match = re.match('^int main\(', line)
        if match:
            types[str(path_bin)] = 'main'
            depends(path_bin, on=path_o)

        match = re.match('^TEST(_F)?\\(', line)
        if match:
            types[str(path_bin)] = 'test'
            depends(path_bin, on=path_o)

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
            object_file = dep.replace('.h', '.o')
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

if args.verbose:
    print('C++ dependency graph')
    for path in sorted(types.keys()):
        print(f' "{path}" : {types[path]}')
        for dep in sorted(graph[path]):
            print(f'  - "{dep}" : {types[dep]}')
