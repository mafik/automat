# Executable Shape

## Goal

The Automat executable is one statically linked binary. On Linux the system clang links it
statically against musl and against libc++, libc++abi, libunwind and the compiler-rt builtins,
which `src/0_musl.py` builds from the LLVM checkout. Every symbol is resolved at link time, and the binary has no dynamic section. The binary loads at a fixed address, so it
needs no relocation pass at startup and its symbol addresses are stable across runs, which
makes debugger output reproducible. Every variant writes full debug information.

The flags that produce this shape live in `run_py/build.py` and `src/0_musl.py`. This document
records why each decision was made.

## Dependencies

Automat must build on any Linux distribution. The build may assume only python, git, a
compiler and the host tools that `run_py/debian_deps.py` lists; every library is downloaded
and built locally by a build extension (`src/*.py`) against musl. Static archives of the host
system are built for the host's C library and are never linked. `src/0_musl.py` writes a clang
configuration file, `build/{variant}/musl.cfg`, which names the musl target and makes
`build/{variant}/sysroot` the sysroot and the resource directory. Every compiler command of the
build and of the dependencies carries it, and `PKG_CONFIG_LIBDIR` names only the build prefix,
so `-lm` or `-latomic` cannot resolve to the host's archives. The kernel headers are the
host's, linked into `build/{variant}/linux_uapi`.

The binary has no shared library dependencies decided at link time. Code that must come from
the host is opened at run time. `automat::crt` (`src/crt.cpp`) maps the host's dynamic
loader through a small helper program that is built for the host (`src/crt.py`) and takes the
host's `dlopen`, `dlsym`, `pthread_create` and `free` from it. Automat defines `dlopen`,
`dlsym`, `dlclose` and `dlerror` itself and forwards them to the host's loader, so statically
linked libraries that open libraries, such as vk-bootstrap opening the host's Vulkan loader,
reach the host's libraries. A `dlsym` lookup with `RTLD_DEFAULT` searches the global scope of
the host's loader, which contains the host's C library. libsystemd looks up newer functions of
the C library that way from static constructors and would call the host's functions, which
report errors through the host's `errno`. For this reason `src/libsystemd.py` patches these
lookups out, and libsystemd makes the system calls directly. A thread that calls into host
libraries is a `crt::ChimeraThread`:
the host's C library creates it and musl attaches its own thread structure to it. `crt`
gives the host's loader a `PT_TLS` entry for Automat's own TLS segment, so the host's C library
allocates that segment in every thread it creates, at the offsets that the static link assumed,
and musl finds its thread structure through a thread-local variable (the source patch in
`src/0_musl.py`).

The host's loader installs its own thread pointer and allocates the TLS block of the thread
that it starts on. musl therefore calls `__start_host` (`src/crt.cpp`), which starts the host's
loader, before it gives the main thread a thread pointer (the source patch in
`src/0_musl.py`). The main thread keeps one thread pointer and one TLS block from then on, so
static constructors may use thread-locals and keep their addresses. `__start_host` runs
without a thread structure: it makes its system calls through `LinuxSyscall` and calls no
function of the C library that uses `errno`, stdio or the thread structure. When it cannot map
the helper or the host's loader, it stores an error text and returns, and the process continues
as a plain musl program. A failure inside the host's loader ends the process. `crt::Init` is
the first statement of `automat::Main` and of the `main` of every test binary: it reports the
stored error, or looks up the host's functions that `crt` calls.

A thread that a host library creates by itself has no musl thread structure, so static code
must not run on it; a signal handler that runs on such a thread faults at its first use of
`errno`. musl is built with unwind tables, including those of its assembly routines, so that a
fault inside a libc function unwinds into the calling frames.

`crt` reports the host's objects through `dl_iterate_phdr` and `dladdr` (`src/crt.cpp`). It
reads the list of objects of the host's loader (`_r_debug`) and calls no function of the host,
so the lookup works on every thread. The unwinder therefore finds the unwind tables of the
host's libraries, and call stacks and crash reports continue through host frames, which
Tracy's symbolizer names.

One unwinder, Automat's (LLVM libunwind), unwinds the exceptions of the whole process. The
personality routine of a host C++ frame belongs to the host's libstdc++ and calls the unwinder
functions of the C++ ABI (`_Unwind_GetLanguageSpecificData`, `_Unwind_SetIP`, `_Unwind_Resume`
and the others) by name. The helper program (`src/crt_helper.c`) defines and exports these
functions. The host's loader searches the main program first, so the host's libraries call the
helper's definitions, and the helper forwards each call to Automat's unwinder. A throw from
Automat's code therefore passes host frames by the host's rules: destructors of host frames
run, a host `catch (...)` catches the exception, and the throw continues into Automat's frames
below. An exception that a host library throws passes Automat's frames in the same way.

glibc does not look its unwinder up by name. `pthread_exit`, `pthread_cancel` and `backtrace`
open `libgcc_s.so.1` with glibc's private `dlopen` and take `_Unwind_ForcedUnwind`,
`_Unwind_Resume`, `_Unwind_GetCFA`, `_Unwind_Backtrace` and `__gcc_personality_v0` from the
object that this name resolves to. The helper therefore takes the name `libgcc_s.so.1` for
itself. Before Automat's code runs, the helper loads
the host's libgcc_s into the global scope and then points the `DT_SONAME` entry of its own
dynamic section at the string `libgcc_s.so.1`. The helper is linked with a placeholder name and
without RELRO, so that this entry is writable. The host's loader compares a requested name with
the loaded objects in their load order, the main program first, and reads each object's
`DT_SONAME` entry at every comparison. Every later request for `libgcc_s.so.1`, from glibc or
from a host library, therefore resolves to the helper, and glibc's forced unwinds run on
Automat's unwinder: the destructors of host frames and of Automat's frames run. A host library
that imports another function of libgcc_s finds it in the host's libgcc_s, because the helper has
no version definitions and the lookup continues through the global scope. glibc before 2.31
stops a versioned lookup at the object that the version entry names, so such a library fails to
load there; `crt::Init` reports glibc versions older than 2.31 as an error. A chimera thread
frees its launch record and detaches musl's thread structure in a destructor, so that a forced
unwind through its body does both.

Automat uses the host's libxcb in this way. Vulkan drivers call back into the XCB connection
that Automat hands them through `VK_KHR_xcb_surface`. The driver's libxcb and Automat's libxcb
must be the same library with the same connection state, so `xcb::LoadHostLibxcb`
(`src/xcb.cpp`) opens the host's `libxcb.so.1` at run time through `automat::crt::OpenLibrary`.
Every core libxcb function is a generated stub (`src/xcb.py`) that calls the host's function.
The generated protocol code of the core protocol and of the extensions (`libxcb.a`,
`libxcb-render.a`, `libxcb-xinput.a` and the others) is linked statically from locally built
archives. No other X library is linked.

Automat also opens the host's libpipewire at run time. libpipewire is a thin client whose
protocol modules and SPA plugins are loaded from the host at run time and are built for the
host's C library, so `Libpipewire::LoadSymbols` (`src/pipewire.cpp`) opens the host's
`libpipewire-0.3.so.0` and looks up the libpipewire functions that Automat calls. Only
Automat's own code calls libpipewire, so it calls these functions through the pointers in
`automat::pw` and no stubs are generated. Only the PipeWire headers are installed
from the PipeWire source; the library itself is not built.
PipeWire's loops run on `crt::ChimeraThread`s that Automat starts, so that Automat's callbacks
never run on a thread that the host's C library created.

## Static archives

The link is static, so the linker resolves every `-lfoo` to an archive. `StaticLibs` in
`src/glib.py` resolves pkg-config expansions at link time and rewrites the libraries in them
(`-lffi`, `-lpcre2-8`, `-lz`) to explicit archive names. It also strips the
`-Wl,--export-dynamic` that gmodule-2.0.pc injects.

## Allocator

Every Linux binary of the build allocates through mimalloc (`src/mimalloc.py`). musl's
allocator takes one lock for every operation, so threads that allocate at the same time wait
for each other. mimalloc keeps a heap for every thread.

mimalloc is linked as one object file, which `src/mimalloc.py` adds to the objects of every
binary. The linker links every object file that it is given and searches `libc.a` only for
names that are still undefined, so no member of `libc.a` that defines an allocation function
is linked. The object defines `malloc`, `free`, `calloc`, `realloc`, the aligned allocation
functions, `malloc_usable_size` and the C++ allocation operators. It also defines
`__libc_malloc`, `__libc_calloc`, `__libc_realloc` and `__libc_free`, the names through which
musl allocates for its own use, so the binary contains no allocator of musl.

The program break belongs to the host's allocator, and static code must not move it. mimalloc
takes its memory from the system with `mmap`. musl's `sbrk` is linked because LLVM refers to
it; it reports the program break and fails for every non-zero increment.

The two allocators do not know each other's memory. Memory that a host library allocated is
freed with `crt::Free`, which calls the host's `free`.

mimalloc finds the heap of a thread through thread-local variables and releases the heap in
the destructor of a thread-specific key of musl. The host's C library allocates the TLS
segment in every thread that it creates, and musl runs the destructors of its keys when one of
its own threads ends and when a `crt::ChimeraThread` ends, so both mechanisms work on the main
thread, on the threads of musl and on every `crt::ChimeraThread`.

## Fixed load address

The binary is linked with `-fno-pie -no-pie` and `-static`, so the linker produces an
executable for a fixed address. A position-independent executable pays for address space randomization
with a startup relocation pass — about 106,000 `R_X86_64_RELATIVE` entries in Automat's case,
one for every address constant in vtables and pointer tables — and with GOT-indirect access to
its own globals. The fixed-address binary has none of that, and every symbol sits at the
address the debugger and `nm` report.

The `asan` and `tsan` variants do not build on Linux (`docs/Stochastic Parrots.md`).

## Debug information

Every Linux variant compiles and links with `-gz=zstd`, so the binary carries its debug
information in compressed sections, which gdb reads natively.
`LowLevelError::FindSourceLocation` (`src/error_recovery.cpp`) reads them through
`/proc/self/exe`, so faults symbolize in every variant.

## Inspecting

`readelf -lS automat` shows the program headers, which have no `INTERP` and no
`DYNAMIC` entry, and the sections. A program header that asks for a dynamic loader is a
regression in this design.

## Windows

The Windows build follows the same intent with PE mechanics. The toolchain is clang with the
MSVC ABI and lld-link; every library is a locally built static archive, linked by explicit
path or through the defaultlib directives embedded in object files. The shared library
dependencies are operating-system DLLs plus the delay-loaded tensorflow.dll
(`src/tensorflow.py`). The C runtime is linked statically, including the UCRT, because an
object file can replace the allocation functions of the static UCRT and cannot replace those
that ucrtbase.dll exports. mimalloc's object (`src/mimalloc.py`) defines them, so the
executable contains no allocator of the UCRT. The debug variant is the exception: mimalloc's
static override does not compile against the debug CRT, so debug binaries allocate through the
CRT. tensorflow.dll allocates from ucrtbase and the executable from mimalloc, so a C++ object
that one module allocates and the other frees is not supported. 128-bit
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
