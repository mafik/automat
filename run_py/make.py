'''Pythonic replacement for GNU Make.'''
# SPDX-FileCopyrightText: Copyright 2024 Automat Authors
# SPDX-License-Identifier: MIT

from pathlib import Path
from collections import defaultdict
from sys import platform
import time
import multiprocessing
import os
import subprocess
import signal
import shutil
import tempfile
import hashlib
import fs_utils
import args as cmdline_args

if platform == 'win32':
    import windows

# Hash directory for the current build variant
import build_variant
if build_variant.current:
    HASH_DIR = build_variant.current.BASE / 'hashes'
else:
    HASH_DIR = fs_utils.build_dir / 'hashes'
HASH_DIR.mkdir(parents=True, exist_ok=True)


def Popen(args, **kwargs):
    '''Wrapper around subprocess.Popen which captures STDERR into a temporary file.'''
    f = tempfile.TemporaryFile()
    str_args = []
    for x in args:
        if callable(x):
            str_args += x()
        else:
            str_args.append(str(x))

    if cmdline_args.args.verbose:
        print(' $ \033[90m' + ' '.join(str_args) + '\033[0m')
    if 'stdout' in kwargs:
        if isinstance(kwargs['stdout'], Path):
            kwargs['stdout'] = kwargs['stdout'].open('w')
        else:
            # keep kwargs['stdout'] as is
            pass
    elif not cmdline_args.args.verbose:
        kwargs['stdout'] = f
    p = subprocess.Popen(str_args,
                         stdin=subprocess.DEVNULL,
                         stderr=f,
                         **kwargs)
    p.stderr = f
    if 'env' in kwargs:
        p.env = kwargs['env']
    return p

def hexdigest(path):
    path = Path(path)
    if path.exists():
        if path.is_dir():
            contents = path.stat().st_mtime_ns.to_bytes(8, 'big')
        else:
            contents = path.read_bytes()
    else:
        contents = b''
    return hashlib.md5(contents).hexdigest()


class Step:

    def __init__(self,
                 build_func,
                 outputs,
                 inputs,
                 id,
                 desc=None,
                 shortcut=None,
                 cleanup=None,
                 stderr_prettifier=lambda x: x):
        if not desc:
            if hasattr(build_func, '__name__'):
                desc = f'Running {build_func.__name__}'
            else:
                raise ValueError(f'Step from module {build_func.__module__} is missing a required "desc" parameter')
        if not shortcut:
            if hasattr(build_func, '__name__'):
                shortcut = f'{build_func.__name__}'
            else:
                raise ValueError(f'Step from module {build_func.__module__} is missing a required "shortcut" parameter')
        if '/' in shortcut:
            raise ValueError(f'Slashes not allowed in step shortcuts: {shortcut}')
        self.desc = desc
        self.shortcut = shortcut
        self.outputs = set(str(x) for x in outputs)
        self.inputs = set(str(x) for x in inputs)
        self.build = build_func  # function that executes this step
        self.builder = None  # Popen instance while this step is being built
        self.id = id
        self.stderr_prettifier = stderr_prettifier
        self.cleanup = cleanup

    def __repr__(self):
        return f'{self.desc}'

    def build_and_log(self, reasons):
        print(f'{self.desc}...')  # , '(because', *reasons, 'changed)')
        return self.build()

    def record_input_hashes(self):
        hash_path = HASH_DIR / self.shortcut
        text = '\n'.join(f'{inp} {hexdigest(inp)}' for inp in self.inputs)
        hash_path.write_text(text)

    def dirty_inputs(self):
        # Check 1: If the output doesn't exist, report that all inputs have changed.
        for out in self.outputs:
            if not Path(out).exists():
                return self.inputs

        # Check 2: Check whether the inputs are older than outputs.
        if self.outputs:
            build_time = min(
                Path(t).stat().st_mtime if Path(t).exists() else 0
                for t in self.outputs)
        else:
            build_time = 0
        updated_inputs = []
        for inp in self.inputs:
            p = Path(inp)
            # Prevent infinite rebuild loops when outputting to an input directory.
            if p.is_dir() and any(Path(out).is_relative_to(p) and Path(out).exists() for out in self.outputs):
                continue
            if p.exists() and p.stat().st_mtime < build_time:
                continue
            if not p.exists():
                continue
            updated_inputs.append(inp)

        if len(updated_inputs) == 0:
            return []

        # Check 3: If possible - check whether the contents have changed.
        hash_path = HASH_DIR / self.shortcut
        if not hash_path.exists():
            return updated_inputs
        recorded_hashes = defaultdict(str)
        for line in hash_path.open().readlines():
            inp, hsh = line.split()
            recorded_hashes[inp] = hsh
        changed_inputs = [
            inp for inp in updated_inputs
            if hexdigest(inp) != recorded_hashes[inp]
        ]
        return changed_inputs

    def build_if_needed(self):
        if len(self.inputs) == 0 and any(not Path(out).exists()
                                         for out in self.outputs):
            if cmdline_args.args.verbose:
                print(f'Running "{self.shortcut}" because some of the outputs don\'t exist')
            return self.build_and_log([])
        if len(self.outputs) == 0:
            if cmdline_args.args.verbose:
                print(f'Running "{self.shortcut}" because it has no outputs (phony target)')
            return self.build_and_log([])
        updated_inputs = self.dirty_inputs()
        if len(updated_inputs) > 0:
            if cmdline_args.args.verbose:
                print(f'Running "{self.shortcut}" because {str(updated_inputs)} changed')
            return self.build_and_log(updated_inputs)


class Recipe:
    steps: list[Step]
    generated: set[str]
    pid_to_step: dict[int, Step]

    def __init__(self):
        '''A list of steps that should be taken in order to build the final product.'''
        self.steps = []
        '''A set of files that have been automatically generated and can be safely removed.'''
        self.generated = set()
        '''Maps PID of a running process to a step.'''
        self.pid_to_step = dict()

    def clean(self):
        '''Removes all of the generated files.'''
        for p in self.generated:
            p = Path(p)
            if p.exists():
                if p.is_file() or p.is_symlink():
                    print(f'  > unlink {p}')
                    p.unlink()
                else:
                    print(f'  > rmtree {p}')
                    shutil.rmtree(p)

    def add_step(self, *args, **kwargs):
        self.steps.append(Step(*args, id=len(self.steps), **kwargs))

    # prunes the list of steps and only leaves the steps that are required for some target
    def set_target(self, target, *extra_targets):
        out_index = defaultdict(list)
        new_steps = set()
        q = []
        for step in self.steps:
            if step.shortcut == target:
                q.append(step)
            for output in step.outputs:
                out_index[output].append(step)

        if len(q) == 0:
            for step in self.steps:
                for output in step.outputs:
                    if str(Path(output).relative_to(fs_utils.project_root)) == target:
                        q.append(step)
                        break

        if len(q) == 0:
            from difflib import get_close_matches
            close = get_close_matches(target, [s.shortcut for s in self.steps])
            if close:
                close = ', '.join(close)
                raise Exception(
                    f'{target} is not a valid target. Close matches: {close}.')
            else:
                targets = ', '.join([s.shortcut for s in self.steps])
                raise Exception(
                    f'{target} is not a valid target. Valid targets: {targets}.'
                )

        # Only add extra_targets after the main one was found
        for step in self.steps:
            if step.shortcut in extra_targets:
                q.append(step)

        while q:
            step = q.pop()
            if step in new_steps:
                continue
            new_steps.add(step)
            for input in step.inputs:
                if input in out_index:
                    for dep in out_index[input]:
                        q.append(dep)
                elif not Path(input).exists():
                    raise Exception(
                        f'"{step.desc}" requires `{input}` but it doesn\'t exist and there is no recipe to build it.'
                    )
        new_steps = list(new_steps)
        new_steps.sort(key=self.steps.index)

        self.steps = new_steps

    def execute(self, watcher):
        start_time = time.time()
        desired_parallelism = multiprocessing.cpu_count()
        ready_steps = []

        for step in self.steps:
            step.blocker_count = 0

        for a in self.steps:
            for b in self.steps:
                if a == b:
                    continue
                if b.outputs & a.inputs:
                    a.blocker_count += 1
            if a.blocker_count == 0:
                ready_steps.append(a)

        def on_step_finished(a):
            a.record_input_hashes()
            for b in self.steps:
                if b.inputs & a.outputs:
                    b.blocker_count -= 1
                    if b.blocker_count == 0:
                        ready_steps.append(b)

        def check_for_pid():
            for pid, step in self.pid_to_step.items():
                status = step.builder.poll()
                if status != None:
                    return pid, status
            if watcher:
                status = watcher.poll()
                if status != None:
                    return watcher.pid, status
            return 0, 0

        def wait_for_pid():
            if platform == 'win32':
                while True:
                    pid, status = check_for_pid()
                    if pid:
                        return pid, status
                    time.sleep(0.01)
            else:
                return os.wait()

        while ready_steps or self.pid_to_step:
            if len(ready_steps) == 0 or len(
                    self.pid_to_step) >= desired_parallelism:
                running_names = ', '.join(
                    [r.shortcut for r in self.pid_to_step.values()])
                print(
                    f'Waiting for one of {len(self.pid_to_step)} running steps ({running_names})...'
                )
                while True:
                    pid, status = wait_for_pid()
                    if watcher and pid == watcher.pid:
                        print('Sources have been modified. Interrupting the build process...')
                        self.interrupt()
                        return False
                    break
                if pid not in self.pid_to_step:
                    print('Warning: unregistered child process has finished. PID=', pid, ', exit status=', status, '. Make sure that all the steps in the build graph properly return their PIDs! You can use the `./run_.py/moniter_new_pids.py` while this command is running to identify extra processes.')
                    continue
                step = self.pid_to_step[pid]
                if status:
                    print(f'{step.desc} finished with an error:\n')
                    if hasattr(step.builder, 'args'):
                        env = ''
                        if hasattr(step.builder, 'env'):
                            for k, v in step.builder.env.items():
                                env += f'   {k}={v}\n'
                            env = env.strip() + '\n   '
                        orig_command = ' > \033[90m' + \
                            env + ' '.join(step.builder.args) + '\033[0m\n'
                        print(orig_command)
                    if step.builder.stderr:
                        step.builder.stderr.seek(0)
                        stderr = step.builder.stderr.read().decode('utf-8')
                        for line in stderr.split('\n'):
                            print('  ' + step.stderr_prettifier(line))
                    else:
                        print('  (no stderr)')
                    if step.cleanup:
                        try:
                            step.cleanup()
                        except Exception as e:
                            print(f'Cleanup failed: {e}')
                    self.interrupt()
                    return False
                step.builder = None
                del self.pid_to_step[pid]
                on_step_finished(step)
            else:
                next = ready_steps.pop()
                try:
                    builder = next.build_if_needed()
                    if builder:
                        next.builder = builder
                        self.pid_to_step[builder.pid] = next
                    else:
                        on_step_finished(next)
                except PermissionError as err:
                    print(f'{next.desc} failed due to a permission error.', err)
                    self.interrupt()
                    return False
                except subprocess.CalledProcessError as err:
                    print(f'{next.desc} finished with an error.', err)
                    self.interrupt()
                    return False
                except FileNotFoundError as err:
                    print(f'{next.desc} couldn\'t find file {err}')
                    self.interrupt()
                    return False
        print(
            f'Build took {time.time() - start_time:.3f} seconds ({len(self.steps)} steps)'
        )
        return True

    def interrupt(self):
        start_time = time.time()
        deadline = start_time + 3
        active = [step.builder for step in self.steps if step.builder]
        if platform == 'win32':
            # Plan:
            # For each process, get its PID. Then find it's HWND and send a WM_CLOSE message.
            for task in active:
                windows.close_window(pid=task.pid)
        else:
            for task in active:
                task.send_signal(signal.SIGINT)
        for task in active:
            time_left = deadline - time.time()
            if time_left > 0:
                try:
                    task.wait(time_left)
                except subprocess.TimeoutExpired:
                    pass # wait for other tasks before killing
        for task in active:
            task.kill()
