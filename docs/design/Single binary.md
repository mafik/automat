# Single binary

A major objective of Automat is simplicity. It's binaries should be as reliable and easy to run as possible. The ultimate version of that, and the ultimate goal of Automat is to have just one binary, that runs on different operating systems, different architectures in different languages etc.

There is space-utility tradeoff there. Extra architectures, extra languages, extra OS compatibility - each of them take precious bytes. This is probably still a good goal to aim for because the amount of bytes consumed by each of those variants is constant and the benefit of having all of them in a single binary is multiplicative.

This is akin to the idea of having a Docker container - although without the need for Docker - and with extreme simplicity for end-user.

This document contains notes about different paths to achieve this goal. Different approaches have been explored to varying degrees. This document should be used as a starting point for pushing this goal further.

## Standard static linking

While this seems like an obvious solution, glibc discourages this (because it leads to inconsistent behavior of DNS resolution and character conversions - which rely on dynamic linking). There is a whole discussion about glibc & static linking, which, sadly, resulted in glibc basically not supporting it.

## Linking on old machines

It's still possible to build a relatively cross-platform binaries - by building on old machines and linking (dynamically) against oldest possible versions of all dependencies. Obviously it's not optimal because it locks Automat out of the bleeding edge features.

## `.symver` directives

Another approach might be to use https://github.com/wheybags/glibc_version_header to link to specific versions of glibc. Unfortunately this doesn't cover the statically linked libraries, that Automat uses - they often still link to some specific version of glibc - and will not run on Linux machines using older versions.

## Stripping version information

The `patchelf` utility can be used to strip symbol versions from ELF binaries. Unfortunately this leads linker to select the oldest version of each symbol, confusing many parts of Automat's code.

This approach *almost* works. Automat starts up and runs, but the timer events seem to cause some issues. It wasn't deeply investigated.

Here is a `run.py` extension that can strip symbol versions from ELF binaries (paste it into a new `.py` file in `src` directory):

```python
import build
import make
from functools import partial
from subprocess import check_output, run

def hook_plan(srcs, objs : list[build.ObjectFile], bins : list[build.Binary], recipe : make.Recipe):
  for bin in bins:
    if not bin.build_type.is_subtype_of(build.release):
      continue

    def clear_symbol_versions(bin):
      # TODO: implement this in python (avoid nm & patchelf)
      nm = check_output(['nm', '-D', '-f', 'just-symbols', str(bin.path)])
      for line in nm.splitlines():
        if b'@' in line:
          sym, _, ver = line.partition(b'@')
          run(['patchelf', '--clear-symbol-version', sym, str(bin.path)], check=True)

    recipe.add_step(
      partial(clear_symbol_versions, bin),
      inputs=[bin.path],
      outputs=[bin.path],
      desc=f'Clearing symbol versions from {bin.path.name}',
      shortcut=f'clear symbol versions from {bin.path.name}')
```

## Cosmopolitan libc

Cosmopolitan libc is probably the way to go. Not only it produces static binaries, but they also work across operating systems. It still needs to be examined. The key question that needs to be answered is - whether it's possible to display a Vulkan surface with it.
