'''Generates embedded*.hpp and embedded*.cpp, with contents of all the static files.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

import re
import fs_utils
import cc_embed
import make
import src
import subprocess

from pathlib import Path
from functools import partial


def slug_from_path(path):
    return re.sub(r'[^a-zA-Z0-9]', '_', str(path))


def escape_string(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')


hpp_path = fs_utils.generated_dir / 'embedded.hpp'
cpp_path = fs_utils.generated_dir / 'embedded.cpp'
n_shards = 16
shard_names = [f'embedded_{i}' for i in range(n_shards)]
shard_embeds = [[] for _ in range(n_shards)]
shard_hpp_path = [fs_utils.generated_dir / f'{shard_names[i]}.hpp' for i in range(n_shards)]
shard_cpp_path = [fs_utils.generated_dir / f'{shard_names[i]}.cpp' for i in range(n_shards)]
embedded_paths_file = fs_utils.generated_dir / 'embedded_paths'

paths = []        # embed list; reset in hook_srcs, extended via inject()
embed_steps = []  # steps that read paths/shard_embeds; see embed_after()

def gen_hpp():
    with hpp_path.open('w') as hpp:
        print('#pragma once', file=hpp)
        print('#include <unordered_map>', file=hpp)
        print('', file=hpp)
        print('#include "../../src/virtual_fs.hpp"', file=hpp)
        for i in range(n_shards):
            print(f'#include "{shard_names[i]}.hpp"  // IWYU pragma: export', file=hpp)
        print('', file=hpp)
        print('namespace automat::embedded {', file=hpp)
        print('', file=hpp)
        print('extern std::unordered_map<StrView, fs::VFile*> index;', file=hpp)
        print('', file=hpp)
        print('}  // namespace automat::embedded', file=hpp)

def gen_cpp():
    with cpp_path.open('w') as cpp:
        print('#include "embedded.hpp"', file=cpp)
        print('', file=cpp)
        print('using namespace std::string_literals;', file=cpp)
        print('using namespace automat;', file=cpp)
        print('using namespace automat::fs;', file=cpp)
        print('', file=cpp)
        print('namespace automat::embedded {', file=cpp)
        print('', file=cpp)
        print('std::unordered_map<StrView, VFile*> index = {', file=cpp)
        for path in paths:
            slug = slug_from_path(path)
            print(f'  {{ {slug}.path, &{slug} }},', file=cpp)
        print('};', file=cpp)
        print('', file=cpp)
        print('}  // namespace automat::embedded', file=cpp)


def gen_shard(i):
    with shard_hpp_path[i].open('w') as hpp:
        print('#pragma once', file=hpp)
        print('', file=hpp)
        print('#include "../../src/virtual_fs.hpp"', file=hpp)
        print('', file=hpp)
        print('namespace automat::embedded {', file=hpp)
        print('', file=hpp)
        for embed in shard_embeds[i]:
            slug = slug_from_path(embed)
            print(f'extern fs::VFile {slug};', file=hpp)
        print('', file=hpp)
        print('}  // namespace automat::embedded', file=hpp)

    with shard_cpp_path[i].open('w') as cpp:
        print(f'#include "{shard_names[i]}.hpp"', file=cpp)
        print('', file=cpp)
        print('using namespace std::string_literals;', file=cpp)
        print('using namespace automat;', file=cpp)
        print('using namespace automat::fs;', file=cpp)
        print('', file=cpp)
        print('namespace automat::embedded {', file=cpp)
        for path in shard_embeds[i]:
            slug = slug_from_path(path)
            escaped_path = escape_string(str(path))
            print(f'''
static constexpr char {slug}_content[] = {{
#embed "{path}"
}};
VFile {slug} = {{
  .path = "{escaped_path}"sv,
  .content = std::string_view({slug}_content, sizeof({slug}_content))
}};''', file=cpp)
        print('', file=cpp)
        print('}  // namespace automat::embedded', file=cpp)


def inject(path):
    '''Add a file to the embed list at execution time (idempotent).'''
    path = Path(path)
    if path in paths:
        return
    paths.append(path)
    shard_embeds[hash(path) % n_shards].append(path)


def embed_after(*deps):
    '''Make the embed generator steps depend on `deps` (i.e. run after them).'''
    for step in embed_steps:
        step.inputs.update(str(d) for d in deps)


def hook_srcs(srcs: dict[str, src.File], recipe: make.Recipe):
    # hook_srcs reruns on every --live reconfigure, so reset the live state.
    paths.clear()
    embed_steps.clear()
    for bucket in shard_embeds:
        bucket.clear()

    result = subprocess.run(['git', 'ls-files'], capture_output=True, text=True)
    path_set = set(Path(p) for p in result.stdout.splitlines() if  Path(p).is_file())
    path_set.update(p.relative_to(fs_utils.project_root) for p in (fs_utils.project_root / 'assets').glob('*') if p.is_file())

    # Filter out paths marked `automat-embed=false` in .gitattributes (e.g. docs/**).
    attrs = subprocess.run(['git', 'check-attr', '--stdin', '-z', 'automat-embed'],
                           input='\0'.join(p.as_posix() for p in path_set),
                           capture_output=True, text=True).stdout.split('\0')
    excluded = {attrs[i] for i in range(0, len(attrs) - 2, 3) if attrs[i + 2] == 'false'}
    paths.extend(p for p in path_set if p.as_posix() not in excluded)

    fs_utils.generated_dir.mkdir(exist_ok=True)

    embedded_paths_file.write_text('\n'.join(str(p) for p in paths))

    recipe.generated.add(str(hpp_path))
    recipe.add_step(gen_hpp, [hpp_path],
                            [Path(__file__), embedded_paths_file],
                            desc='Writing embedded.hpp',
                            shortcut='embedded.hpp')
    hpp_file = src.File(hpp_path)
    hpp_file.direct_includes.append(str(fs_utils.src_dir / 'virtual_fs.hpp'))
    for h in shard_hpp_path:
        hpp_file.direct_includes.append(str(h))
    srcs[str(hpp_path)] = hpp_file

    recipe.generated.add(str(cpp_path))
    embed_steps.append(recipe.add_step(gen_cpp, [cpp_path],
                                [Path(__file__), embedded_paths_file],
                                desc='Writing embedded.cpp',
                                shortcut='embedded.cpp'))
    cpp_file = src.File(cpp_path)
    cpp_file.direct_includes.append(str(hpp_path))
    srcs[str(cpp_path)] = cpp_file

    for path in paths:
        shard_embeds[hash(path) % n_shards].append(path)

    for i in range(n_shards):
        recipe.generated.add(str(shard_hpp_path[i]))
        recipe.generated.add(str(shard_cpp_path[i]))
        embed_steps.append(recipe.add_step(partial(gen_shard, i),
                        [shard_hpp_path[i], shard_cpp_path[i]],
                        [Path(__file__), embedded_paths_file],
                        desc=f'Writing embedded shard {i}',
                        shortcut=f'embedded_{i}'))

        sh_hpp_file = src.File(shard_hpp_path[i])
        sh_hpp_file.direct_includes.append(str(fs_utils.src_dir / 'virtual_fs.hpp'))
        srcs[str(shard_hpp_path[i])] = sh_hpp_file

        sh_cpp_file = src.File(shard_cpp_path[i])
        sh_cpp_file.direct_includes.append(str(shard_hpp_path[i]))
        for embed in shard_embeds[i]:
            sh_cpp_file.embeds.append(str(embed))
        srcs[str(shard_cpp_path[i])] = sh_cpp_file
