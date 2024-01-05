'''Defines what files are built.'''

from pathlib import Path
from itertools import product
import src
import fs_utils
import make
import os
import functools
import json
from args import args
from dataclasses import dataclass
from sys import platform


class ObjectFile:
    path: Path
    deps: set[src.File]
    source: src.File
    compile_args: list[str]
    build_type: str

    def __init__(self, path: Path):
        self.path = path
        self.deps = set()
        self.build_type = ''
        self.compile_args = []

    def __str__(self) -> str:
        return str(self.path)

    def __repr__(self) -> str:
        return f'ObjectFile({self.path})'


class Binary:
    path: Path
    objects: list[ObjectFile]
    link_args: list[str]
    run_args: list[str]
    build_type: str

    def __init__(self, path: Path):
        self.path = path
        self.objects = []
        self.link_args = []
        self.run_args = []
        self.build_type = ''

    def __str__(self) -> str:
        return str(self.path)

    def __repr__(self) -> str:
        return f'Binary({self.path}, {self.objects}, {self.link_args}, {self.run_args}, {self.build_type})'


build_type_list = ('', 'debug', 'release')

OBJ_DIR = fs_utils.build_dir / 'obj'
OBJ_DIR.mkdir(parents=True, exist_ok=True)


def obj_path(src_path: Path, build_type: str = '') -> Path:
    if build_type:
        return OBJ_DIR / (build_type + '_' + src_path.stem + '.o')
    else:
        return OBJ_DIR / (src_path.stem + '.o')
    
def libname(name):
    return f'{name}.lib' if platform == 'win32' else f'lib{name}.a'

binary_extension = '.exe' if platform == 'win32' else ''

def plan(srcs) -> tuple[list[ObjectFile], list[Binary]]:

    objs: dict[str, ObjectFile] = dict()
    sources = [f for f in srcs.values() if f.is_source()]
    for src_file, build_type in product(sources, build_type_list):
        f_obj = ObjectFile(obj_path(src_file.path, build_type))
        objs[str(f_obj.path)] = f_obj
        f_obj.deps = set(src_file.transitive_includes)
        f_obj.deps.add(src_file)
        f_obj.source = src_file
        f_obj.build_type = build_type
        f_obj.compile_args += src_file.build_compile_args(build_type)
        for inc in src_file.transitive_includes:
            f_obj.compile_args += inc.build_compile_args(build_type)

    binaries: list[Binary] = []
    main_sources = [f for f in sources if f.main]
    for src_file, build_type in product(main_sources, build_type_list):
        bin_name = src_file.path.stem
        if build_type:
            bin_name = build_type + '_' + bin_name
        bin_path = fs_utils.build_dir / bin_name
        if binary_extension:
            bin_path = bin_path.with_suffix(binary_extension)
        bin_file = Binary(bin_path)
        bin_file.build_type = build_type
        binaries.append(bin_file)

        queue: list[src.File] = [src_file]
        visited: set[src.File] = set()
        while queue:
            f = queue.pop()
            if f in visited:
                continue
            visited.add(f)
            bin_file.link_args += f.build_link_args(build_type)
            bin_file.run_args += f.build_run_args(build_type)
            if f_obj := objs.get(str(obj_path(f.path, build_type)), None):
                if f_obj not in bin_file.objects:
                    bin_file.objects.append(f_obj)
            queue.extend(f.transitive_includes)
            if f_cc := srcs.get(str(f.path.with_suffix('.cc')), None):
                queue.append(f_cc)

    return list(objs.values()), binaries


compiler = os.environ['CXX'] = os.environ['CXX'] if 'CXX' in os.environ else 'clang++'
compiler_c = os.environ['CC'] = os.environ['CC'] if 'CC' in os.environ else 'clang'

default_compile_args = [
    '-static',
    '-std=gnu++2c', '-fcolor-diagnostics', '-ffunction-sections',
    '-fdata-sections', '-funsigned-char', '-D_FORTIFY_SOURCE=2', '-Wformat',
    '-Wformat-security', '-Werror=format-security', '-fno-plt', '-Wno-vla-extension', '-Wno-trigraphs'
]
release_compile_args = [
    '-O3',
    '-DNDEBUG',
    '-flto',
    '-fstack-protector',
]
debug_compile_args = ['-O0', '-g', '-D_DEBUG', '-fno-omit-frame-pointer']

default_link_args = ['-static', '-fuse-ld=lld']
release_link_args = ['-flto']
debug_link_args = []

if 'CXXFLAGS' in os.environ:
    default_compile_args += os.environ['CXXFLAGS'].split()

if 'LDFLAGS' in os.environ:
    for flag in os.environ['LDFLAGS'].split():
        default_link_args.append(f'-Wl,{flag}')

if platform == 'win32':
    default_link_args += ['-Wl,/opt:ref', '-Wl,/opt:icf']
    debug_link_args += ['-Wl,/debug']
else:
    default_link_args += ['-Wl,--gc-sections', '-Wl,--build-id=none']
    release_link_args += ['-Wl,--strip-all', '-Wl,-z,relro', '-Wl,-z,now']

if False:
    debug_compile_args += ['-fsanitize=address', '-fsanitize-address-use-after-return=always']
    debug_link_args += ['-fsanitize=address']

# Ubuntu 20.04 has some troubles when statically linking to Vulkan
if platform == 'linux':
    default_compile_args = [x for x in default_compile_args if x != '-static']
    default_link_args = [x for x in default_link_args if x != '-static']

if 'g++' in compiler and 'clang' not in compiler:
    # GCC doesn't support -fcolor-diagnostics
    default_compile_args.remove('-fcolor-diagnostics')

if 'OPENWRT_BUILD' in os.environ:
    # OpenWRT has issues with -static C++ builds
    # https://github.com/openwrt/openwrt/issues/6710
    default_link_args.append('-lgcc_pic')
    # OpenWRT doesn't come with lld
    default_link_args.remove('-fuse-ld=lld')

if args.verbose:
    default_compile_args.append('-v')

@dataclass
class CompilationEntry:
    file: str
    output: str
    arguments: list


def recipe() -> make.Recipe:
    r = make.Recipe()
    extensions = src.load_extensions()

    for ext in extensions:
        if hasattr(ext, 'hook_recipe'):
            ext.hook_recipe(r)

    srcs = src.scan()

    for ext in extensions:
        if hasattr(ext, 'hook_srcs'):
            ext.hook_srcs(srcs, r)

    for file in srcs.values():
        file.update_transitive_includes(srcs)

    objs, bins = plan(srcs)

    for ext in extensions:
        if hasattr(ext, 'hook_plan'):
            ext.hook_plan(srcs, objs, bins, r)

    compilation_db = []
    for obj in objs:
        pargs = [compiler] + default_compile_args
        if obj.build_type == 'debug':
            pargs += debug_compile_args
        elif obj.build_type == 'release':
            pargs += release_compile_args
        if obj.source.path.name.endswith('.c'):
            pargs[0] = compiler_c
            pargs = [x for x in pargs if x != '-std=gnu++2c']

        pargs += obj.compile_args
        pargs += [str(obj.source.path)]
        pargs += ['-c', '-o', str(obj.path)]
        builder = functools.partial(make.Popen, pargs)
        r.add_step(builder,
                   outputs=[obj.path],
                   inputs=obj.deps | set(['compile_commands.json']),
                   desc=f'Compiling {obj.path.name}',
                   shortcut=obj.path.name)
        r.generated.add(obj.path)
        compilation_db.append(
            CompilationEntry(str(obj.source.path), str(obj.path), pargs))
    for bin in bins:
        pargs = [compiler]
        pargs += [str(obj.path) for obj in bin.objects]
        pargs += default_link_args
        if bin.build_type == 'debug':
            pargs += debug_link_args
        elif bin.build_type == 'release':
            pargs += release_link_args
        pargs += bin.link_args
        pargs += ['-o', str(bin.path)]
        builder = functools.partial(make.Popen, pargs)
        r.add_step(builder,
                   outputs=[bin.path],
                   inputs=bin.objects,
                   desc=f'Linking {bin.path.name}',
                   shortcut=f'link {bin.path.name}')
        r.generated.add(bin.path)

        # if platform == 'win32':
        #     MT = 'C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.19041.0\\x64\\mt.exe'
        #     mt_runner = functools.partial(Popen, [MT, '-manifest', 'src\win32.manifest', '-outputresource:{path}'])
        #     r.add_step(mt_runner, outputs=[path], inputs=[path, 'src/win32.manifest'], name=f'mt {binary_name}')

        runner = functools.partial(make.Popen, [bin.path] + bin.run_args)
        r.add_step(runner,
                   outputs=[],
                   inputs=[bin.path],
                   desc=f'Running {bin.path.name}',
                   shortcut=bin.path.name)

    for ext in extensions:
        if hasattr(ext, 'hook_final'):
            ext.hook_final(srcs, objs, bins, r)

    def compile_commands():
        jsons = []
        for entry in compilation_db:
            arguments = ',\n    '.join(
                json.dumps(str(arg)) for arg in entry.arguments)
            json_entry = f'''{{
  "directory": { json.dumps(str(fs_utils.project_root)) },
  "file": { json.dumps(entry.file) },
  "output": { json.dumps(entry.output) },
  "arguments": [{arguments}]
}}'''
            jsons.append(json_entry)
        with open('compile_commands.json', 'w') as f:
            print('[' + ', '.join(jsons) + ']', file=f)

    r.add_step(compile_commands, ['compile_commands.json'], [],
               desc='Writing JSON Compilation Database',
               shortcut='compile_commands.json')
    r.generated.add('compile_commands.json')

    return r
