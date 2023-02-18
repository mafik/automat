'''Module which keeps track of all C++ sources.'''

import fs_utils
import collections
import re

srcs = []
for ext in ['.cc', '.ixx']:
    srcs.extend(fs_utils.project_root.glob(f'src/**/*{ext}'))

graph = collections.defaultdict(set)
types = dict()

def depends(what, on):
    graph[str(what)].add(str(on))

for path_abs in srcs:
    parent = path_abs.parent
    path = path_abs.relative_to(fs_utils.project_root)
    path_pcm = path.with_suffix('.pcm')
    path_bin = path.with_suffix('')

    if path.suffix == '.cc':
        path_o = path.with_name(path.stem + '_cc.o')
        types[str(path)] = 'UNKNOWN' # should be overwritten based on module declaration
        types[str(path_o)] = 'object file'
        depends(path_o, on=path)
        target = path_o
    elif path.suffix == '.ixx':
        path_o = path.with_name(path.stem + '_pcm.o')
        types[str(path_o)] = 'object file'
        types[str(path_pcm)] = 'precompiled module'
        # Module interfaces produce .pcm files
        depends(path_pcm, on=path)
        # PCM files produce .o files
        depends(path_o, on=path_pcm)
        target = path_pcm

    if path.name.endswith('_test.cc'):
        types[str(path)] = 'test source'
        types[str(path_bin)] = 'test'
        depends(path_bin, on=path_o)

    if path.stem.startswith('main'):
        types[str(path)] = 'main source'
        types[str(path_bin)] = 'main'
        depends(path_bin, on=path_o)

    for line in open(path_abs).readlines():
        match = re.match('^import ([a-zA-Z0-9_]+);', line)
        if match: # Import module
            dep = path.with_name(match.group(1) + '.pcm')
            depends(target, on=dep)
        
        match = re.match('^import <([a-zA-Z0-9_/\.-]+)>;', line)
        if match: # Import system header unit
            dep = match.group(1) + '.pcm'
            types[dep] = 'system header unit'
            depends(target, on=dep)
        
        match = re.match('^import \"([a-zA-Z0-9_/\.-]+)\";', line)
        if match: # Import user header unit
            dep = match.group(1) + '.pcm'
            types[dep] = 'user header unit'
            depends(target, on=dep)

        match = re.match('^module ([a-zA-Z0-9_]+);', line)
        if match:
            types[str(path)] = 'module implementation'
            assert path.name == match.group(1) + '.cc', 'All modules should be implemented in <module_name>.cc files'
            # Module implemenations need their PCM (precompiled module) file
            depends(path_o, on=path_pcm)

        match = re.match('^export module ([a-zA-Z0-9_]+);', line)
        if match: # Module interface
            types[str(path)] = 'module interface'
            assert path.name == match.group(1) + '.ixx', 'All modules interfaces should be in <module_name>.ixx files'

for path, t in types.items():
    if t not in ('test', 'main'):
        continue
    deps = list(graph[path])
    visited = set()
    while len(deps) > 0:
        dep, deps = deps[0], deps[1:]
        if dep in visited:
            continue
        visited.add(dep)
        if types[dep] == 'precompiled module':
            module_pcm_o = dep.replace('.pcm', '_pcm.o')
            module_cc_o = dep.replace('.pcm', '_cc.o')
            depends(path, on=module_pcm_o)
            deps.append(module_pcm_o)
            if module_cc_o in graph:
                depends(path, on=module_cc_o)
                deps.append(module_cc_o)
        deps.extend(graph[dep])
