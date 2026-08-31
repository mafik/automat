# Error Recovery

## Goal

Automat should keep running, and recover, when errors happen inside Objects. The Error model
is outlined in the comment block at the top of `src/error.hpp`; parts of it are marked as not
implemented and its shape is expected to change. The low-level layer described below is
the first part of that design: it turns a hardware fault inside an executing Object into an
Error attached to that Object.

## Errors

`src/error.hpp` defines the model:

- An Error explains to the user what went wrong and helps with recovery. Errors are attached
  to Objects; each Object has at most one Error.
- While present, an Error pauses the execution of its Object. Each Object is responsible for
  checking its Error when it executes. `RunTask::DoneRunning` in `src/tasks.cpp` schedules an
  Object's successors only when `HasError` reports no Error.
- Errors may be attached by external reporters, which work like validators. An Error records
  its reporter, which is usually the target Object itself. `Object::ReportError`
  (`src/object.hpp`) reports an Error whose reporter is the Object; `automat::ReportError`
  (`src/error.hpp`) reports one with a different reporter.
- Errors are cleared by the user or by their reporter. Errors caused by failed preconditions
  clear themselves when the Object executes again.
- Marked as not implemented in `src/error.hpp`: Errors that keep otherwise deleted Objects
  alive, Error watchers, and the visualization of reporters and of messages. The comment
  describes the intended visualization as fire with a smoke bubble explaining the issue.
  `ErrorFlames` (`src/error_flames.hpp`) draws fire around the outline of the Object and the
  Error text in red beneath it. It is a detached child of the Object's toy, created, refreshed
  and destroyed by `ObjectToy::UpdateErrorFlames`, which `ObjectToy::OnPoll` calls whenever
  `Toy::Poll` (`src/toy.cpp`) observes a change of the Object's wake counter; `ManipulateError`
  bumps that counter after every change, so the flames follow the Error without polling the
  error list and without involving the Location. It has no shape of its own, so it never
  reacts to the pointer; its draw bounds are the bounds of the toy's shape extended by the
  reach of the fire and by the text. It shares the toy's local space, so `PackFrame`
  (`src/renderer.cpp`) gives it the toy's texture anchors and the compositor warps it together
  with the toy.

All Errors are stored in one vector guarded by a mutex in `src/error.cpp`. `ManipulateError`
is the thread-safe primitive; `HasError`, `ReportError` and `ClearError` are built on it.

## The task boundary

`AutomatLoop` in `src/tasks.cpp` is the worker thread loop that executes tasks. It captures the
task's target Object before executing the task and runs the task under `ERROR_RECOVERY_TRY`.
The handler block formats the fault description together with the instruction pointer,
resolves the source location with `FindSourceLocation` (falling back to an empty
`SourceLocation`), and calls `ReportError` on the target. When the target is gone, the fault
is logged instead.

## Low-level recovery

`src/error_recovery.hpp` declares the interface, which is the same on every platform:
`LowLevelError`, the per-thread `SignalStack`, `Init` and `Stop`. `src/error_recovery.cpp`
shares the symbolizer state, `FindSourceLocation` and the frames of `Init` and `Stop` between
platforms, and holds the fault interception behind platform conditionals: the Linux part
converts SIGSEGV, SIGBUS, SIGILL and SIGFPE into a thrown `LowLevelError`; the Windows part
converts the corresponding Windows exceptions. Both parts re-aim the faulting instruction
pointer with their platform's `UnwindReturnAddress` before the throw, so that the faulting
frame's destructors run; the shared rule is described in the Linux section and the Windows
section points out the differences. The recovered cases are reads, writes and executions of protected memory
(a mapping that lacks the required permission) and of unmapped memory, unaligned accesses,
unknown instructions, arithmetic errors, and stack overflow.

### LowLevelError

`LowLevelError` holds a type and the faulting instruction pointer; `Type::NONE` marks an
empty instance. Moving from a `LowLevelError` empties it, and destroying an instance that
holds `STACK_OVERFLOW` restores the stack guard. It is deliberately minimal: throwing `Status` or `SourceLocation` from the handler is too heavy.
`Description` of the fault is produced at the task boundary; `FindSourceLocation` resolves
the instruction pointer with `LLVMSymbolizer` under a mutex, and `error_recovery::Init` loads
the module information once so that the first fault does not wait for it. `Init` also records
the executable path the symbolizer reads; before it runs, `FindSourceLocation` reports
nothing. On Linux the
symbolizer reads `/proc/self/exe` and takes the address unchanged, because the binary loads
at a fixed address. On Windows the symbolizer reads the executable and its PDB, and the
address is also taken unchanged, because the executable links as a fixed-base image and
always loads at its preferred base (`docs/parrots/Executable Shape.md`). A design that
converted addresses at run time could not even read the preferred base from the running
process, because the loader rewrites the `ImageBase` field of the in-memory headers to the
actual base. Every Linux variant, including release, embeds zstd-compressed debug information
in the binary, and every Windows variant writes a PDB (`docs/parrots/Executable Shape.md`), so
faults symbolize in every variant. When the running binary has no debug information to offer,
`FindSourceLocation` reports nothing and the fault message keeps the address.

### Catching a LowLevelError

Code inside a real `catch` block for these errors runs before the stack has recovered: the
runtime releases the overflowed stack region only when the catch block exits, and it destroys
the thrown object from the dispatcher's frame at the depth of the fault. The guard page can
only be restored after the catch block, and any further work inside it risks a second, fatal
overflow. `ERROR_RECOVERY_TRY` and `ERROR_RECOVERY_CATCH` (`src/error_recovery.hpp`)
therefore replace `try` and `catch`: the real catch block only moves the thrown error into an
instance declared before the `try`, and the caller's handler block runs after the catch has
exited, on a consolidated stack. Because moving empties the source, the thrown object's
destructor does nothing at the dangerous position, and the moved-to instance restores the
guard when it is destroyed at the end of the enclosing scope.

### Per-thread setup

`error_recovery::SignalStack` is the per-thread object that prepares a thread for recovery;
`AutomatLoop` declares one at the start of each worker thread, and the test suite's
environment holds one for its main thread. On Linux it is an RAII object with inline storage
that registers itself with `sigaltstack` for its lifetime, giving signal delivery a stack to
run on when the thread's own stack overflows; the handlers themselves apply to every thread,
so a thread without a `SignalStack` still recovers from everything except stack overflow. On
Windows it calls `SetThreadStackGuarantee`, which reserves space at the bottom of the stack
for exception dispatch during a stack overflow, and records the thread id in a fixed table of
atomic slots; only threads in the table recover. The table exists because foreign code
(drivers, COM, the runtime) raises and handles access violations internally with structured
exception handling, and a handler that intercepted those on every thread would break them.

### The signal handler (Linux)

`ErrorRecoverySignalHandler` classifies the signal (`ClassifySignal`), re-aims the saved
instruction pointer (`UnwindReturnAddress`, described below), unblocks the signal and throws
the `LowLevelError`. Throwing from the handler works because GCC's stack unwinder can
properly unwind signal handler frames. When the fault is an execution of protected or unmapped
memory, the handler first replaces the saved instruction pointer with the return address found
at the saved stack pointer and pops it, so that unwinding resumes in the caller of the
faulting call.

A SIGSEGV is classified along two axes. Whether the memory is protected or unmapped comes from
`si_code`: `SEGV_ACCERR` and `SEGV_PKUERR` mean a mapping exists but denies the access, and
every other code is treated as unmapped, which also covers the general protection fault that a
non-canonical address raises (`SI_KERNEL`, with no fault address). Whether the access was a
read, a write or an instruction fetch comes from the page fault error code that the kernel
saves in `REG_ERR`. The hardware "page present" bit is not used, because Linux keeps the page
table entries of `PROT_NONE` mappings non-present, so it would report the guard page of a
stack as unmapped.

A SIGSEGV is classified as a stack overflow (`IsStackOverflow`) when the faulting address lies
within one page of the saved stack pointer, on either side. The check uses only the registers
saved by the kernel; it does not look up the thread's stack bounds. An overflowing access is a
push, a red zone access, or a store into a frame that the stack pointer has already been moved
past, so it lies close to the stack pointer, while a wild pointer is unrelated to it. Frames
larger than a page may fault further away from the stack pointer and are then reported as an
ordinary memory access; the build deliberately does not probe stack pages
(`-fstack-clash-protection`), so that stack allocation stays free. The check runs after the
execution check, because executing protected memory is the more specific report, and before
the other SIGSEGV classifications, because the guard region of a pthread stack would otherwise
be reported as a write to protected memory (glibc maps it `PROT_NONE`) or to unmapped memory
(on Linux 6.13 and later, glibc installs it with `MADV_GUARD_INSTALL`, and the kernel reports
a fault there as `SEGV_MAPERR`).

### The unwind return address (Linux)

The compiler records exception-table entries only around calls, so the table misdescribes the
unwinding obligations of a faulting instruction between calls: the fault lies either outside
every call-site entry, or inside an entry with no landing pad, recorded for a call that
genuinely needs no cleanup (for example a constructor call, before which there is nothing to
destroy yet). Unwinding from the faulting instruction pointer as it is would skip the
destructors of the faulting frame. `UnwindReturnAddress` therefore re-aims the saved
instruction pointer at the start of the next call-site entry after the fault, and the stock
personality applies that entry's rule during the throw.

The next call site is the right source of cleanups because destruction obligations of
interest are anchored at calls: a mutex is held after its lock call returned, so the unlock
cleanup is recorded from the following call site onward. This keeps a fault under a lock from
leaking the lock and hanging whoever waits for it, including third-party code whose locks
Automat cannot know about. The remaining imprecision is RAII whose liveness changes through
fully inlined code between the fault and the surrounding calls; its cleanup can be run
spuriously or missed. That imprecision is accepted until Clang implements
`-fnon-call-exceptions`, which records the tables exactly. Windows reaches the same decision
over its IP-to-state map, described below.

`UnwindReturnAddress` reaches the faulting frame's exception table with `_Unwind_Backtrace`:
the walk crosses the signal frame, the first frame reported with the ip-before-instruction
flag is the faulting frame, and the table is available there through
`_Unwind_GetLanguageSpecificData` and `_Unwind_GetRegionStart`. The saved instruction pointer
is kept when the covering call-site entry has a landing pad, because the table's rule for
that region already applies. It is also kept when the table cannot be parsed or when no entry
after the fault qualifies. An entry qualifies when it has no action chain or its action chain
contains a cleanup, so a handler-only entry (for example the terminate path of a noexcept
function) is never chosen and the frame is skipped through the next qualifying entry instead.
The thrown `LowLevelError` carries the original instruction pointer, so the fault is reported
and symbolized where it happened.

The stock personality terminates the process when a frame's instruction pointer is not
covered by any call-site entry of its exception table. The re-aiming keeps that from
happening for the layouts the compiler emits today, but a fault where no later entry
qualifies (for example inside a landing pad, while a destructor faults during unwinding)
could still present an uncovered instruction pointer. The Linux part is therefore linked with
`-Wl,--wrap=__gxx_personality_v0`; `__wrap___gxx_personality_v0` defers covered instruction
pointers to the stock personality unchanged and reports "continue unwinding" for uncovered
ones, so such a frame is skipped instead of terminating the process.

### The exception handler (Windows)

`ErrorRecoveryExceptionHandler` is a vectored exception handler installed at the front of the
chain (`AddVectoredExceptionHandler(1, ...)`), so it sees the exception before any frame-based
handler. It handles `EXCEPTION_ACCESS_VIOLATION`, `EXCEPTION_STACK_OVERFLOW`,
`EXCEPTION_ILLEGAL_INSTRUCTION`, `EXCEPTION_PRIV_INSTRUCTION`,
`EXCEPTION_INT_DIVIDE_BY_ZERO`, `EXCEPTION_INT_OVERFLOW`, `EXCEPTION_IN_PAGE_ERROR` and
`EXCEPTION_DATATYPE_MISALIGNMENT`, only on registered threads. It also declines while a C++
exception is unwinding (`std::uncaught_exceptions`), because a destructor faulting during
unwinding could only produce a second throw, which terminates the process; declining delivers
the same end more directly, as an ordinary fatal fault.

An access violation is classified along two axes: `ExceptionInformation[0]` distinguishes
read, write and execute, and `VirtualQuery` on the faulting address distinguishes protected
memory (`MEM_COMMIT`) from unmapped memory (`MEM_FREE` or `MEM_RESERVE`).
`EXCEPTION_STACK_OVERFLOW` is reported by the OS directly, so no heuristic is involved.
`EXCEPTION_IN_PAGE_ERROR` and `EXCEPTION_DATATYPE_MISALIGNMENT` map to
`ACCESSED_UNALIGNED_MEMORY`, the closest analog of SIGBUS; no user-mode scenario raises
either of them deterministically on x86-64 (setting the alignment check flag in EFLAGS does
not cause faults on this configuration), so no test covers them.

Throwing a C++ exception from inside a vectored handler is outside the platform contract.
Instead the handler modifies the saved thread context so that the thread resumes as if the
faulting instruction had called a function that throws: it pushes the faulting instruction
pointer as a return address, places the fault type and address in the first two argument
registers, points the instruction pointer at `ThrowLowLevelError` and returns
`EXCEPTION_CONTINUE_EXECUTION`. The unwinder then treats the faulting function as the
thrower's caller. The thrower declares an over-aligned local, which makes the compiler emit a
stack-realigning prologue with `UWOP_SET_FPREG` unwind info, so it tolerates whatever stack
alignment the fault interrupted. The register spill space of the simulated call lands in the
faulting frame's outgoing-argument area, or among the dead locals of a leaf frame, so no live
data is overwritten.

An execution fault needs no return-address fixup on Windows: the faulting instruction pointer
has no function table entry, and the unwinder treats such a frame as a leaf function, popping
the return address that the faulting call pushed.

The return address the handler pushes is not the faulting instruction pointer, because the
exception states that map instruction pointers to live objects are recorded at call
boundaries: the state recorded for an instruction between calls lags behind the objects that
actually exist, and the faulting frame's own destructors would be skipped.
`UnwindReturnAddress` pushes the address of the next state transition instead. It resolves the
faulting instruction pointer with `RtlLookupFunctionEntry`, follows chained unwind information
to the entry that carries the handler, requires that handler to be `__CxxFrameHandler3`, reads
the `FuncInfo` its handler data points at, and takes the first entry of the IP-to-state map
that lies after the fault and still inside the same function. The state recorded there
includes every object whose construction finished before the fault, because a transition is
recorded at the call that finishes it. The thrown `LowLevelError` still carries the original
instruction pointer, so the fault is reported and symbolized where it happened.

The original instruction pointer is pushed unchanged whenever that lookup does not apply: the
instruction pointer lies outside every module, the function has no exception handler or a
handler other than `__CxxFrameHandler3`, the `FuncInfo` carries no recognized magic number, or
no IP-to-state entry follows the fault inside the function. The remaining imprecision is the
same as on Linux: RAII whose liveness changes through fully inlined code between the fault and
the surrounding calls can have its destructor run spuriously or missed.

### Stack overflow (Windows)

When a stack overflow is delivered, the guarantee region reserved by `SetThreadStackGuarantee`
is committed, and the handler, the thrower and the exception dispatch run inside it. Unwinding
moves the stack pointer back up, so destructors and the catch run on ordinary committed
stack. The consumed guard region is not restored automatically, and without restoration the
next overflow on the thread kills the process. The destructor of a `LowLevelError`
holding a stack overflow calls `_resetstkoflw`, which reinstates the guard page, and reports
failure, because the reset can fail even when correctly placed. The catching pattern above
times that destruction to happen after the catch block; a call made any earlier fails and
restores nothing. On Linux the destructor has nothing to restore, because the alternate
signal stack and the guard region survive recovery.

### Lifecycle

`error_recovery::Init` installs the handlers process-wide and loads the symbolizer's module
information; `automat::Main` calls it before `StartWorkerThreads`. `error_recovery::Stop`
restores the previous signal dispositions on Linux and removes the vectored exception handler
on Windows; `automat::Main` calls it after `JoinWorkerThreads`.

### SourceLocation

`automat::SourceLocation` (`src/source_location.hpp`) is a 24-byte type that converts to and
from `std::source_location` and can own its strings. libstdc++'s `std::source_location` holds
a pointer to a record; converting an owning `SourceLocation` there allocates a record that is
never freed, because `std::source_location` can only view its data. The MSVC STL stores its own record
by value, with the line and column ahead of the file and function pointers, so the conversion
fills that layout directly and only the strings of an owning `SourceLocation` are leaked. The `owned` flag is a `U32` bitfield rather than a `bool`,
because the MSVC ABI never packs bitfields of different types into one storage unit and the
type is asserted to stay 24 bytes.

### Fault addresses

The fault message reports an address inside Automat's own image as the raw virtual address:
the image loads at its link-time base, so that address is exactly what offline symbolization
takes. `InMainExecutable` (`src/memory.cpp`) checks it against the bounds the linker
provides - `__executable_start` and `_end` on Linux, `__ImageBase` plus the image size from
its headers on Windows. Faults in other modules still move between runs, so `ResolveAddress`
qualifies them with the module path and the offset from the module base - from
`/proc/self/maps` on Linux and `GetModuleHandleExW` on Windows. Addresses outside any module
keep an empty file name and the raw address.

### Tests

`src/error_recovery_test.cpp` covers the recovered cases and the `LowLevelError::Type` each of
them produces, stack overflow on the main thread and on a worker thread, a second overflow
after the first one's error has been destroyed, a wild read into the region guarding the stack that must not count
as a stack overflow, and the effect of `Stop`. Every scenario reaches its fault through a call
on a volatile function pointer; without that barrier the optimizer proves that the try block
cannot throw and deletes the exception handling that the recovered exception needs. Death
tests on Windows re-execute the test binary for every test, so the suite environment runs in
each child; it suppresses the error-reporting dialog (`SetErrorMode`) and disables gtest's
structured exception interception, which would otherwise catch the intentionally fatal
scenarios inside the child instead of letting it die. The destructor-faulting-during-unwinding
scenario is recovered on Linux and fatal on Windows; the SIGBUS scenario exists only on
Linux.
