'''Defines what files are built.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT
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

TRIPLE = 'x86_64-pc-linux-gnu'

class BuildType:
    def __init__(self, name, base=None, is_default=False):
        self.name = name
        self.name_lower = name.lower()
        self.base = base

        # Callables in the argument lists (`compile_args`, `link_args`) will be
        # invoked with the build type as an argument. Everything else will be
        # converted to strings.
        self.compile_args = []
        self.link_args = []
        self.is_default = is_default

        gcc_arch_dir = self.PREFIX() / 'lib' / 'gcc' / TRIPLE
        if gcc_arch_dir.exists():
            # TODO: support versions like 10.3.0
            gcc_version = max(int(x.name) for x in gcc_arch_dir.iterdir() if x.is_dir())
            gcc_dir = gcc_arch_dir / str(gcc_version)
            if args.verbose:
                print(f'{self.name} build using GCC', gcc_version, 'from', gcc_dir)
            self.compile_args += [f'--gcc-install-dir={gcc_dir}']
            self.link_args += [f'--gcc-install-dir={gcc_dir}']
        elif args.verbose:
            print(f'{self.name} build using system-provided GCC. Build `gcc{self.rule_suffix()}` to create a custom GCC installation.')

        self.compile_args += [f'-I{self.PREFIX()}/include']
        self.link_args += [f'-L{self.PREFIX()}/lib']
        self.link_args += [f'-L{self.PREFIX()}/lib64']
        self.PREFIX().mkdir(parents=True, exist_ok=True)
        (self.PREFIX() / 'bin').mkdir(parents=True, exist_ok=True)

    def is_subtype_of(self, other):
        if self == other:
            return True
        if self.base:
            return self.base.is_subtype_of(other)
        return False
    
    def rule_suffix(self):
        return '' if self.is_default else f'_{self.name_lower}'
    
    def BASE(self):
        return (fs_utils.build_dir / self.name).absolute()
    
    def PREFIX(self): # TODO: change this to a member variable
        return self.BASE() / 'PREFIX'
    
    def CXXFLAGS(self):
        return [str(x) for x in (self.base.CXXFLAGS() if self.base else []) + self.compile_args]

    def CFLAGS(self):
        return [x for x in self.CXXFLAGS() if x != '-std=gnu++26']
    
    def LDFLAGS(self):
        return [str(x) for x in (self.base.LDFLAGS() if self.base else []) + self.link_args]
    
    def __getattr__(self, name):
        # if this object doesn't have the attribute, try to get it from the base
        if self.base:
            return getattr(self.base, name)
    
    def __str__(self):
        return self.name
    
    def __repr__(self):
        return f'BuildType({self.name})'
    

# Common config for all build types
base = BuildType('Base', is_default=True)

base.compile_args += ['-static', '-std=gnu++26', '-fcolor-diagnostics', '-ffunction-sections',
    '-fdata-sections', '-funsigned-char', '-fno-signed-zeros',
    '-fno-strict-aliasing', '-fno-exceptions',
    '-D_FORTIFY_SOURCE=2', '-Wformat', '-Wno-c99-designator',
    '-Wformat-security', '-Werror=format-security', '-Wno-vla-extension', '-Wno-trigraphs', '-Werror=return-type',
    '-gsplit-dwarf']

if platform != 'win32':
    # On PE/COFF, functions cannot be interposed (https://maskray.me/blog/2021-05-09-fno-semantic-interposition)
    base.compile_args += ['-fno-semantic-interposition']
    # We don't want PLT in our ELF binary
    base.compile_args += ['-fno-plt']


if 'CXXFLAGS' in os.environ:
    base.compile_args += os.environ['CXXFLAGS'].split()

base.link_args += ['-static', '-fuse-ld=lld']

if 'LDFLAGS' in os.environ:
    for flag in os.environ['LDFLAGS'].split():
        base.link_args.append(f'-Wl,{flag}')

# Build type optimized for fast incremental builds
fast = BuildType('Fast', base)
fast.compile_args += ['-O1', '-g']

# Build type intended for practical usage (slow to build but very high performance)
release = BuildType('Release', base)
release.compile_args += ['-O3', '-DNDEBUG', '-flto', '-fstack-protector', '-fno-trapping-math']
release.link_args += ['-flto']

# Build type intended for debugging
debug = BuildType('Debug', base)
debug.compile_args += ['-O0', '-g', '-D_DEBUG', '-fno-omit-frame-pointer']

default = fast

types = [fast, release, debug]

if platform == 'linux':
    asan = BuildType('ASan', debug)
    asan.compile_args += ['-fsanitize=address', '-fsanitize-address-use-after-return=always']
    asan.link_args += ['-fsanitize=address']
    types.append(asan)

    # As of 2024-09-30, libnvidia-glcore.so bypasses pthread initialization which breaks
    # tsan. One workaround might be to disable GPU rendering and try CPU-based rendering.
    # https://github.com/google/sanitizers/issues/1647
    tsan = BuildType('TSan', debug)
    tsan.compile_args += ['-fsanitize=thread', '-DCPU_RENDERING']
    tsan.link_args += ['-fsanitize=thread']
    types.append(tsan)

    # TODO(custom libc++ in place): enable Memory Sanitizer
    # Memory Sanitizer requires us to build all dependencies with MSan enabled
    # Skia instructions: https://skia.org/docs/dev/testing/xsan/
    # msan = BuildType('MSan', debug)
    # msan.compile_args += ['-fsanitize=memory', '-fsanitize-memory-track-origins']
    # msan.link_args += ['-fsanitize=memory']
    # types.append(msan)

    ubsan = BuildType('UBSan', debug)
    ubsan.compile_args += ['-fsanitize=undefined']
    ubsan.link_args += ['-fsanitize=undefined']
    types.append(ubsan)

class ObjectFile:
    path: Path
    deps: set[src.File]
    source: src.File
    compile_args: list[str]
    build_type: BuildType

    def __init__(self, path: Path):
        self.path = path
        self.deps = set()
        self.build_type = default
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
    build_type: BuildType

    def __init__(self, path: Path):
        self.path = path
        self.objects = []
        self.link_args = []
        self.run_args = []
        self.build_type = default

    def __str__(self) -> str:
        return str(self.path)

    def __repr__(self) -> str:
        return f'Binary({self.path}, {self.objects}, {self.link_args}, {self.run_args}, {self.build_type})'


OBJ_DIR = fs_utils.build_dir / 'obj'
OBJ_DIR.mkdir(parents=True, exist_ok=True)


def obj_path(src_path: Path, build_type: BuildType = default) -> Path:
    if build_type == default:
        return OBJ_DIR / (src_path.stem + '.o')
    else:
        return OBJ_DIR / (build_type.name_lower + '_' + src_path.stem + '.o')
    
def libname(name):
    return f'{name}.lib' if platform == 'win32' else f'lib{name}.a'

# TODO: replace all usages with fs_utils.binary_extension
binary_extension = fs_utils.binary_extension

def plan(srcs) -> tuple[list[ObjectFile], list[Binary]]:

    objs: dict[str, ObjectFile] = dict()
    sources = [f for f in srcs.values() if f.is_source()]
    for src_file, build_type in product(sources, types):
        f_obj = ObjectFile(obj_path(src_file.path, build_type))
        objs[str(f_obj.path)] = f_obj
        f_obj.deps = set(src_file.transitive_includes)
        f_obj.deps.add(src_file)
        # TODO: maybe instead of depending on this file, it would be possible
        # to depend on the specific compile_args that were used?
        f_obj.deps.add('run_py/build.py')
        f_obj.source = src_file
        f_obj.build_type = build_type
        f_obj.compile_args += src_file.build_compile_args(build_type.name_lower)
        for inc in src_file.transitive_includes:
            f_obj.compile_args += inc.build_compile_args(build_type.name_lower)

    binaries: list[Binary] = []
    main_sources = [f for f in sources if f.main]
    for src_file, build_type in product(main_sources, types):
        bin_name = src_file.path.stem
        if build_type != default:
            bin_name = build_type.name_lower + '_' + bin_name
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
            bin_file.link_args += f.build_link_args(build_type.name_lower)
            bin_file.run_args += f.build_run_args(build_type.name_lower)
            if f_obj := objs.get(str(obj_path(f.path, build_type)), None):
                if f_obj not in bin_file.objects:
                    bin_file.objects.append(f_obj)
            queue.extend(f.transitive_includes)
            if f_cc := srcs.get(str(f.path.with_suffix('.cc')), None):
                queue.append(f_cc)

    return list(objs.values()), binaries


compiler = os.environ['CXX'] = os.environ['CXX'] if 'CXX' in os.environ else 'clang++'
compiler_c = os.environ['CC'] = os.environ['CC'] if 'CC' in os.environ else 'clang'

if platform == 'win32':
    base.compile_args += ['-D_USE_MATH_DEFINES', '-DNODRAWTEXT']
    base.link_args += ['-Wl,/opt:ref', '-Wl,/opt:icf']
    debug.link_args += ['-Wl,/debug']
else:
    base.link_args += ['-Wl,--gc-sections', '-Wl,--build-id=none', '-Wl,-z,relro', '-Wl,-z,now']
    release.link_args += ['-Wl,--strip-all']


if 'g++' in compiler and 'clang' not in compiler:
    # GCC doesn't support -fcolor-diagnostics
    base.compile_args.remove('-fcolor-diagnostics')

if 'OPENWRT_BUILD' in os.environ:
    # OpenWRT has issues with -static C++ builds
    # https://github.com/openwrt/openwrt/issues/6710
    base.link_args.append('-lgcc_pic')
    # OpenWRT doesn't come with lld
    base.link_args.remove('-fuse-ld=lld')

if args.verbose:
    base.compile_args.append('-v')

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

    import ninja
    ninja.hook_recipe(r)

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

    COMPILE_COMMANDS_PATH = fs_utils.project_root / 'compile_commands.json'
    compilation_db = []
    for obj in objs:
        if obj.source.path.name.endswith('.c'):
            pargs = [compiler_c] + obj.build_type.CFLAGS()
        else:
            pargs = [compiler] + obj.build_type.CXXFLAGS()

        for arg in obj.compile_args:
            if callable(arg):
                pargs.append(functools.partial(arg, obj.build_type))
            else:
                pargs.append(arg)
        pargs += [str(obj.source.path)]
        pargs += ['-c', '-o', str(obj.path)]
        builder = functools.partial(make.Popen, pargs)
        r.add_step(builder,
                   outputs=[obj.path],
                   inputs=obj.deps | set([str(COMPILE_COMMANDS_PATH)]),
                   desc=f'Compiling {obj.path.name}',
                   shortcut=obj.path.name)
        r.generated.add(str(obj.path))
        expanded_args = []
        for arg in pargs:
            if callable(arg):
                expanded_args += arg()
            else:
                expanded_args.append(arg)
        compilation_db.append(
            CompilationEntry(str(obj.source.path), str(obj.path), expanded_args))
    for bin in bins:
        pargs = [compiler]
        pargs += [str(obj.path) for obj in bin.objects]
        pargs += bin.build_type.LDFLAGS()
        for arg in bin.link_args:
            if callable(arg):
                pargs.append(functools.partial(arg, bin.build_type))
            else:
                pargs.append(arg)
        pargs += ['-o', str(bin.path)]
        builder = functools.partial(make.Popen, pargs)
        r.add_step(builder,
                   outputs=[bin.path],
                   inputs=bin.objects,
                   desc=f'Linking {bin.path.name}',
                   shortcut=f'link {bin.path.stem}')
        r.generated.add(str(bin.path))

        # if platform == 'win32':
        #     MT = 'C:\\Program Files (x86)\\Windows Kits\\10\\bin\\10.0.19041.0\\x64\\mt.exe'
        #     mt_runner = functools.partial(Popen, [MT, '-manifest', 'src\win32.manifest', '-outputresource:{path}'])
        #     r.add_step(mt_runner, outputs=[path], inputs=[path, 'src/win32.manifest'], name=f'mt {binary_name}')

        runner = functools.partial(make.Popen, [bin.path] + bin.run_args)
        r.add_step(runner,
                   outputs=[],
                   inputs=[bin.path],
                   desc=f'Running {bin.path.name}',
                   shortcut=bin.path.stem)

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
        with COMPILE_COMMANDS_PATH.open('w') as f:
            print('[' + ', '.join(jsons) + ']', file=f)

    r.add_step(compile_commands, [COMPILE_COMMANDS_PATH], [],
               desc='Writing JSON Compilation Database',
               shortcut='compile_commands.json')
    r.generated.add(str(COMPILE_COMMANDS_PATH))

    def deploy():
        return make.Popen(['rsync', '--protect-args', '-av', '--delete', '--exclude', 'builds', '--exclude', 'assets', '-og', '--chown=maf:www-data', 'www/', 'protectli:/var/www/automat.org/'], shell=False)


    r.add_step(deploy, [], ['www/'], desc='Uploading WWW contents to server')


    return r
