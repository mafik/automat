'''Generates embedded*.hh and embedded*.cc, with contents of all the static files.'''
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


hh_path = fs_utils.generated_dir / 'embedded.hh'
cc_path = fs_utils.generated_dir / 'embedded.cc'
n_shards = 16
shard_names = [f'embedded_{i}' for i in range(n_shards)]
shard_embeds = [[] for _ in range(n_shards)]
shard_hh_path = [fs_utils.generated_dir / f'{shard_names[i]}.hh' for i in range(n_shards)]
shard_cc_path = [fs_utils.generated_dir / f'{shard_names[i]}.cc' for i in range(n_shards)]
embedded_paths_file = fs_utils.generated_dir / 'embedded_paths'

def gen_hh():
    with hh_path.open('w') as hh:
        print('#pragma once', file=hh)
        print('#include <unordered_map>', file=hh)
        print('', file=hh)
        print('#include "../../src/virtual_fs.hh"', file=hh)
        for i in range(n_shards):
            print(f'#include "{shard_names[i]}.hh"', file=hh)
        print('', file=hh)
        print('namespace automat::embedded {', file=hh)
        print('', file=hh)
        print('extern std::unordered_map<StrView, fs::VFile*> index;', file=hh)
        print('', file=hh)
        print('}  // namespace automat::embedded', file=hh)

def gen_cc(all_paths):
    with cc_path.open('w') as cc:
        print('#include "embedded.hh"', file=cc)
        print('', file=cc)
        print('using namespace std::string_literals;', file=cc)
        print('using namespace automat;', file=cc)
        print('using namespace automat::fs;', file=cc)
        print('', file=cc)
        print('namespace automat::embedded {', file=cc)
        print('', file=cc)
        print('std::unordered_map<StrView, VFile*> index = {', file=cc)
        for path in all_paths:
            slug = slug_from_path(path)
            print(f'  {{ {slug}.path, &{slug} }},', file=cc)
        print('};', file=cc)
        print('', file=cc)
        print('}  // namespace automat::embedded', file=cc)


def gen_shard(i):
    with shard_hh_path[i].open('w') as hh:
        print('#pragma once', file=hh)
        print('', file=hh)
        print('#include "../../src/virtual_fs.hh"', file=hh)
        print('', file=hh)
        print('namespace automat::embedded {', file=hh)
        print('', file=hh)
        for embed in shard_embeds[i]:
            slug = slug_from_path(embed)
            print(f'extern fs::VFile {slug};', file=hh)
        print('', file=hh)
        print('}  // namespace automat::embedded', file=hh)

    with shard_cc_path[i].open('w') as cc:
        print(f'#include "{shard_names[i]}.hh"', file=cc)
        print('', file=cc)
        print('using namespace std::string_literals;', file=cc)
        print('using namespace automat;', file=cc)
        print('using namespace automat::fs;', file=cc)
        print('', file=cc)
        print('namespace automat::embedded {', file=cc)
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
}};''', file=cc)
        print('', file=cc)
        print('}  // namespace automat::embedded', file=cc)


main_step = None


def hook_srcs(srcs: dict[str, src.File], recipe: make.Recipe):
    result = subprocess.run(['git', 'ls-files'], capture_output=True, text=True)
    paths = [Path(p) for p in result.stdout.splitlines() if Path(p).is_file()]

    # Filter out paths marked `automat-embed=false` in .gitattributes (e.g. docs/**).
    attrs = subprocess.run(['git', 'check-attr', '--stdin', '-z', 'automat-embed'],
                           input='\0'.join(p.as_posix() for p in paths),
                           capture_output=True, text=True).stdout.split('\0')
    excluded = {attrs[i] for i in range(0, len(attrs) - 2, 3) if attrs[i + 2] == 'false'}
    paths = [p for p in paths if p.as_posix() not in excluded]

    fs_utils.generated_dir.mkdir(exist_ok=True)

    embedded_paths_file.write_text('\n'.join(str(p) for p in paths))

    recipe.generated.add(str(hh_path))
    recipe.add_step(gen_hh, [hh_path],
                            [Path(__file__), embedded_paths_file],
                            desc='Writing embedded.hh',
                            shortcut='embedded.hh')
    hh_file = src.File(hh_path)
    hh_file.direct_includes.append(str(fs_utils.src_dir / 'virtual_fs.hh'))
    for h in shard_hh_path:
        hh_file.direct_includes.append(str(h))
    srcs[str(hh_path)] = hh_file

    global main_step
    recipe.generated.add(str(cc_path))
    main_step = recipe.add_step(partial(gen_cc, paths), [cc_path],
                                [Path(__file__), embedded_paths_file],
                                desc='Writing embedded.cc',
                                shortcut='embedded.cc')
    cc_file = src.File(cc_path)
    cc_file.direct_includes.append(str(hh_path))
    srcs[str(cc_path)] = cc_file

    global shard_embeds
    for path in paths:
        shard_embeds[hash(path) % n_shards].append(path)

    for i in range(n_shards):
        recipe.generated.add(str(shard_hh_path[i]))
        recipe.generated.add(str(shard_cc_path[i]))
        recipe.add_step(partial(gen_shard, i),
                        [shard_hh_path[i], shard_cc_path[i]],
                        [Path(__file__), embedded_paths_file],
                        desc=f'Writing embedded shard {i}',
                        shortcut=f'embedded_{i}')

        sh_hh_file = src.File(shard_hh_path[i])
        sh_hh_file.direct_includes.append(str(fs_utils.src_dir / 'virtual_fs.hh'))
        srcs[str(shard_hh_path[i])] = sh_hh_file

        sh_cc_file = src.File(shard_cc_path[i])
        sh_cc_file.direct_includes.append(str(shard_hh_path[i]))
        for embed in shard_embeds[i]:
            sh_cc_file.embeds.append(str(embed))
        srcs[str(shard_cc_path[i])] = sh_cc_file
