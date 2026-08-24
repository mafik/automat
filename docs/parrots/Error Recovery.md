# Error Recovery

## Goal

Automat should keep running, and recover, when errors happen inside Objects. The Error model
is outlined in the comment block at the top of `src/error.hpp`; parts of it are marked as not
implemented and its shape is expected to change. The signal-based layer described below is
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
task's target Object before executing the task, runs the task inside a `try` block, and
catches `LowLevelError`. The catch formats the fault description together with the instruction
pointer, resolves the source location with `FindSourceLocation` (falling back to an empty
`SourceLocation`), and calls `ReportError` on the target. When the target is gone, the fault
is logged instead.

## Low-level recovery

`src/error_recovery.hpp` and `src/error_recovery.cpp` convert SIGSEGV, SIGBUS, SIGILL and
SIGFPE into a thrown `LowLevelError`. The recovered cases are reads, writes and executions of
protected memory (a mapping that lacks the required permission) and of unmapped memory,
unaligned accesses, unknown instructions, arithmetic errors, and stack overflow.

### LowLevelError

`LowLevelError` holds a type and the faulting instruction pointer. It is deliberately
minimal: throwing `Status` or `SourceLocation` from the signal handler is too heavy.
`Description` names the fault; `FindSourceLocation` resolves the instruction pointer with
`LLVMSymbolizer` against `/proc/self/exe`, under a mutex. `error_recovery::Init` loads the
module information once so that the first fault does not wait for it.

### The signal handler

`ErrorRecoverySignalHandler` classifies the signal (`ClassifySignal`), unblocks the signal and
throws the `LowLevelError`. Throwing from the handler works because GCC's stack unwinder can
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

### The personality wrapper

`src/error_recovery.cpp` is linked with `-Wl,--wrap=__gxx_personality_v0`.
`__wrap___gxx_personality_v0` parses the frame's exception table and, when the frame's
instruction pointer is not covered by any call-site entry, returns `_URC_CONTINUE_UNWIND`.
The unwinder then skips the frame without running its destructors, instead of the termination
that the stock personality would perform. Skipping those destructors is an accepted compromise
until Clang implements `-fnon-call-exceptions`.

### Signal stacks

`error_recovery::SignalStack` is a per-thread stack for signal delivery, required for stack
overflow recovery. It is an RAII object with inline storage that registers itself with
`sigaltstack` for its lifetime. `AutomatLoop` declares one at the start of each worker thread.
The main thread does not need stack overflow protection and has none.

### Lifecycle

`error_recovery::Init` installs the handlers process-wide with `SA_SIGINFO | SA_ONSTACK`;
`automat::Main` calls it before `StartWorkerThreads`. `error_recovery::Stop` restores the
previous signal dispositions; `automat::Main` calls it after `JoinWorkerThreads`.

### SourceLocation

`automat::SourceLocation` (`src/source_location.hpp`) is a 24-byte type that converts to and
from `std::source_location` and can own its strings. Converting an owning `SourceLocation` to
`std::source_location` allocates a record that is never freed, because `std::source_location`
can only view its data.

### Tests

`src/error_recovery_test.cpp` covers the recovered cases, the `LowLevelError::Type` each of
them produces, stack overflow on both the main thread and a worker thread, a wild access below
the stack that must not count as a stack overflow, and the effect of `Stop`.
