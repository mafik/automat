# Executable Shape

## Goal

The Automat executable should be as close to a statically linked binary as possible. Every
symbol that can be resolved at link time is resolved at link time. The dynamic symbol table
carries no symbols beyond what the dynamic linking model itself requires. The binary loads at
a fixed address, so it needs no relocation pass at startup and its symbol addresses are stable
across runs, which makes debugger output reproducible. Every variant carries full debug
information embedded in the binary; the release variant strips the symbol tables and keeps
only the DWARF.

The flags that produce this shape live in `run_py/build.py`. This document records why each
decision was made and which dependencies are exempt.

## Dependencies

Automat must build on any Linux distribution. The build may assume only python, git and a
compiler; every library is downloaded and built locally by a build extension (`src/*.py`).
Local builds are what make the shape controllable: an OS-provided library dictates whether it
is available as a static archive, a locally built one does not. Three libraries still come
from the OS as static archives — libffi, libpcre2 and libcap — and should eventually get their
own extensions.

The binary keeps exactly five shared library dependencies, each deliberate:

- **libc, libm, ld-linux** — glibc must stay dynamic because the GPU drivers that Vulkan
  loads with `dlopen` crash under a statically linked glibc. This constraint is documented
  where it is enforced, in `src/skia.py`, which strips `-static` from the link and substitutes
  `-static-libstdc++ -static-libgcc`.
- **libxcb** — Vulkan drivers call back into the XCB connection that Automat hands them
  through `VK_KHR_xcb_surface`. The driver's libxcb and Automat's libxcb must be the same
  library with the same connection state, so libxcb is linked shared (`src/xcb.py`). All other
  X libraries (xcb-util, xcb-cursor, libX11, and the rest) are static archives built locally.
- **libpipewire** — the interface to the host's audio and video session. The library is a
  thin client whose protocol modules are loaded from the host at runtime, so linking it
  statically would still depend on the host installation while hiding that dependency.

`-Wl,--as-needed` keeps this list closed: a library that satisfies no import is not recorded,
so transitive `-lfoo` flags from pkg-config expansions cannot grow the dependency list.

## Static archives must be named explicitly

When a directory holds both `libfoo.a` and `libfoo.so`, plain `-lfoo` picks the shared one.
Every link argument for a locally built library therefore names the archive: `-l:libfoo.a`.
The places where this matters:

- `src/zlib.py` deletes the shared zlib that zlib's CMake unconditionally builds, and links
  `-l:libz.a`.
- `src/libsystemd.py` links `-l:libsystemd.a` (meson installs the shared library next to it).
  `src/sdbus_cpp.py` reaches it through `LinkDependsOn`, which appends the dependency's link
  arguments after the consumer's archive, in lazy-archive resolution order.
- `StaticLibs` in `src/glib.py` resolves pkg-config expansions at link time and rewrites the
  system libraries in them (`-lffi`, `-lpcre2-8`, `-lz`) to explicit archive names. It also
  strips the `-Wl,--export-dynamic` that gmodule-2.0.pc injects; the comment in that function
  explains the failure it prevents.

## Fixed load address

The binary links with `-fno-pie -no-pie`. A position-independent executable pays for address
space randomization with a startup relocation pass — about 106,000 `R_X86_64_RELATIVE` entries
in Automat's case, one for every address constant in vtables and pointer tables — and with
GOT-indirect access to its own globals. The fixed-address binary has none of that: the
relocation section shrinks from 2.5 MB to about 7 KB, and every symbol sits at the address the
debugger and `nm` report.

The accepted cost is the standard non-PIE artifact set: a handful of `R_X86_64_COPY`
relocations and their symbols in the dynamic table (`environ`, `stdout`, `stderr`, the
pipewire log globals). These are how a fixed-address executable shares data objects with
shared libraries; they are not internal symbols.

Sanitizer variants (asan, tsan, ubsan) keep the default PIE because ThreadSanitizer's shadow
memory layout assumes it.

## Dynamic symbol table

Nothing exports symbols on purpose. `-Wl,--exclude-libs,ALL` hides every symbol that enters
the link through a static archive, which is how the weak `backtrace` from the vendored
libunwind stays private. The Tracy profiler defines `dlclose` as an interposer; it is compiled
in everywhere except release (`src/tracy.py`), so the release binary's dynamic symbols are the
copy-relocated data objects and nothing else.

## Debug information

Every variant compiles and links with `-gz=zstd`: DWARF5, compressed about 2.6x (667 MB to
258 MB in the fast variant). gdb reads the compressed sections natively. All variants,
including release, keep the debug information embedded in the binary, so
`LowLevelError::FindSourceLocation` (`src/error_recovery.cpp`) can symbolize faults in every
variant and a released binary can be debugged as shipped.

The Linux release additionally drops the linker symbol tables, whose strings cost more than
the compressed DWARF and duplicate what the DWARF already knows. `-Wl,--exclude-libs,ALL`
localizes archive symbols only after the linker's own `--discard-all` filtering, so no link
flag can drop them; instead the linker writes `automat.unstripped` and a separate build step
produces the released binary with `strip --strip-all --keep-section=.debug_*`. The `link
automat` shortcut names the stripping step, so every caller, including `src/release.py` on
the build slaves, receives the stripped binary.

## Inspecting

`readelf -d` shows the dependency list and `BIND_NOW`; `nm -D --defined-only` lists the
dynamic exports; `readelf -rW` counts the relocations. A regression in any of the three is a
regression in this design.

## Windows

The Windows build follows the same intent with PE mechanics. The toolchain is clang with the
MSVC ABI and lld-link; every library is a locally built static archive, linked by explicit
path or through the defaultlib directives embedded in object files. The shared library
dependencies are operating-system DLLs plus the delay-loaded tensorflow.dll
(`src/tensorflow.py`). The C runtime is hybrid: vcruntime and the STL are linked statically,
while malloc comes from the OS-provided ucrtbase, so automat.exe and tensorflow.dll share one
heap and C++ objects can cross between them (the flags live in `run_py/build.py`). 128-bit
arithmetic helpers that the MSVC runtime lacks come from compiler-rt's builtins archive.

Clang emits CodeView debug information and every variant links with `-Wl,/debug`, which
merges the CodeView from all objects into automat.pdb next to the executable; error
recovery's symbolization reads it (`docs/parrots/Error Recovery.md`). Writing the 400 MB PDB
adds about five seconds to the link. The PDB is the platform's separate-debug-file
convention: the executable itself carries no debug information and shipping the PDB is
optional. The symbolizer accepts a PDB found through the path recorded in the executable or
next to the executable, and only when its identity matches; without it, fault reports keep
addresses for offline symbolization. `/opt:ref` and `/opt:icf` stay explicit so that
`/debug` does not disable them; the release link also writes a .map file for binary size
attribution.

The executable links with `/fixed`, mirroring the fixed load address of the ELF build: the
image always loads at its preferred base of 0x140000000, carries no base relocation section,
and its symbol addresses are stable across runs, so error recovery symbolizes raw virtual
addresses exactly as on Linux. High-entropy address space layout randomization is switched
off together with the dynamic base, since neither can apply to an image without
relocations.

`llvm-readobj --file-headers` shows the image base and the DLL characteristics;
`llvm-objdump -h` lists the sections. The binary itself stays free of debug information; the
PDB is a separate file.
