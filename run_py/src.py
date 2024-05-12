'''Functions related to the `src/` directory.'''

from collections import defaultdict
from pathlib import Path
from types import ModuleType
import clang
import fs_utils
import importlib.util
import re
import sys


class File:
    path: Path
    system_includes: list[str]
    comment_libs: list[str]
    direct_includes: list[str]
    transitive_includes: set['File']
    link_args: dict[str, list[str]]
    compile_args: dict[str, list[str]]
    run_args: dict[str, list[str]]
    main: bool

    def __init__(self, path):
        self.path = path
        self.system_includes = []
        self.comment_libs = []
        self.direct_includes = []
        self.link_args = defaultdict(list)
        self.compile_args = defaultdict(list)
        self.run_args = defaultdict(list)
        self.main = False
        self.transitive_includes = set()

    def is_header(self) -> bool:
        return self.path.suffix in ('.h', '.hh')

    def is_source(self) -> bool:
        return self.path.suffix in ('.c', '.cc')

    def build_link_args(self, build_type: str) -> list[str]:
        return self.link_args.get(build_type, []) + (self.link_args.get('', []) if build_type else [])

    def build_compile_args(self, build_type: str) -> list[str]:
        return self.compile_args.get(build_type, []) + (self.compile_args.get('', []) if build_type else [])

    def build_run_args(self, build_type: str) -> list[str]:
        return self.run_args.get(build_type, []) + (self.run_args.get('', []) if build_type else [])

    def scan_contents(self):
        self.direct_includes.clear()
        self.link_args.clear()
        self.compile_args.clear()
        self.run_args.clear()
        self.main = False

        if_stack = [True]
        current_defines = clang.default_defines.copy()

        for line in open(self.path, encoding='utf-8').readlines():

            # Minimal preprocessor. This allows us to skip platform-specific imports.

            # This regular experession captures most of #if defined/#ifdef variants in one go.
            # ?: at the beginning of a group means that it's non-capturing
            # ?P<...> ate the beginning of a group assigns it a name
            match = re.match(
                r'^#(?P<el>el(?P<else>se)?)?(?P<end>end)?if(?P<neg1>n)?(?:def)? ?(?P<neg2>!)?(?:defined)?(?:\()?(?P<id>[a-zA-Z0-9_]+)?(?:\))?', line)
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
            match = re.match(r'^#include <([a-zA-Z0-9_/\.-]+)>', line)
            if match:
                # system include
                self.system_includes.append(match.group(1))
                continue

            match = re.match(r'^#pragma comment\(lib, "([a-zA-Z0-9_/\.-]+)"\)', line)
            if match:
                # extra library
                self.comment_libs.append(match.group(1))
                continue


            match = re.match(r'^#include \"([a-zA-Z0-9_/\.-]+\.hh?)\"', line)
            if match:
                # relative to current source file
                dep = self.path.parent / match.group(1)
                dep = fs_utils.relative_to_root(dep)  # normalize
                self.direct_includes.append(str(dep))

            match = re.match(
                r'^#pragma maf add (?P<build_type>debug|release|fast|) ?(?P<target>link|compile|run) argument "(?P<arg>.+)"', line)
            if match:
                build_type, target, arg = match.groups()
                if target == 'link':
                    target_dict = self.link_args
                elif target == 'compile':
                    target_dict = self.compile_args
                elif target == 'run':
                    target_dict = self.run_args
                else:
                    raise ValueError(f'Unknown target: [{target}] in [{line}]')
                target_dict[build_type].append(arg)

            match = re.match(r'^#pragma maf main', line)
            if match:
                self.main = True

    # This should be called after all files are scanned
    def update_transitive_includes(self, srcs: dict[str, 'File']):
        self.transitive_includes.clear()
        include_queue: list[str] = list(self.direct_includes)
        while include_queue:
            path = include_queue.pop()
            if path not in srcs:
                print(
                    f'Warning: {self.path.name} includes non-existent "{path}"')
                continue
            inc = srcs[path]
            if inc in self.transitive_includes:
                continue
            self.transitive_includes.add(inc)
            for sys in inc.system_includes:
                if sys not in self.system_includes:
                    self.system_includes.append(sys)
            self.main = self.main or inc.main  # propagate `main` flag from headers to sources
            include_queue.extend(inc.direct_includes)

    def __str__(self) -> str:
        return str(self.path)

    def __repr__(self) -> str:
        return f'File({self.path})'


def scan() -> dict[str, File]:
    result = dict()
    paths = []
    for ext in ['.cc', '.hh', '.h', '.c']:
        paths.extend(fs_utils.src_dir.glob(f'**/*{ext}'))

    for path_abs in paths:
        path = path_abs.relative_to(fs_utils.project_root)
        file = File(path)
        result[str(path)] = file
        file.scan_contents()

    return result


def load_extensions() -> list[ModuleType]:
    extensions = []
    old_dont_write_bytecode = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    for path in fs_utils.src_dir.glob('*.py'):
        spec = importlib.util.spec_from_file_location(path.stem, path)
        if not spec:
            continue
        module = importlib.util.module_from_spec(spec)
        if not spec.loader:
            continue
        extensions.append(module)
        spec.loader.exec_module(module)
    sys.dont_write_bytecode = old_dont_write_bytecode
    return extensions
