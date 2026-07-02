#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "base.hpp"
#include "status.hpp"
#include "str.hpp"
#include "stream.hpp"
#include "vec.hpp"

namespace automat::library {

// Splits a line into words on spaces - no quoting, no expansion. Used where
// flat text enters the argv world (SetText, legacy saved states); arguments
// that should contain spaces must arrive as whole argv elements instead.
Vec<Str> SplitWords(StrView line);

// The Command plate's footprint on the board; spawned windows are seated
// next to it.
Vec2 CommandPlateSize();

// File descriptors to install on the child's stdio at spawn (dup2 through
// posix_spawn_file_actions). -1 leaves the descriptor inherited.
struct StdioFds {
  int in = -1;
  int out = -1;
};

// Spawns argv[0] via posix_spawnp (PATH search) with the Wayland compositor
// environment injected: WAYLAND_DISPLAY points at Automat's socket and
// DISPLAY is dropped. Empty elements are skipped. Returns the child pid, or
// 0 with `status` describing the failure.
I64 SpawnArgv(const Vec<Str>& argv, Status& status, const StdioFds& stdio = {});

// Command holds one program invocation - an executable name plus arguments -
// and runs it as a child process. There is no shell anywhere: `argv` reaches
// posix_spawnp exactly as stored, so quoting, globs and $variables do not
// exist. Elements may contain spaces (one element = one argument, always);
// empty elements are transient editor state, skipped at spawn and save.
struct Command : Object {
  mutable std::mutex mutex;  // guards argv and the child bookkeeping
  Vec<Str> argv;             // argv[0] is the program

  // Child bookkeeping, guarded by `mutex`:
  I64 child_pid = 0;    // 0 = no live child
  int wait_status = 0;  // raw waitpid() status of the last finished child
  bool ever_ran = false;

  // stdout stream metering, guarded by `mutex`. Sampled on every UI tick.
  // io_wchar holds the last /proc/<pid>/io wchar reading, kept after exit so
  // the totals freeze instead of dropping to zero. wchar counts the whole
  // process's writes, not one descriptor, so rates from it are process
  // totals. child_pidfd allows transient pidfd_getfd sampling of the pipe
  // (fill and capacity) without Automat ever holding a pipe end.
  uint64_t io_wchar = 0;
  int child_pidfd = -1;
  bool stdout_piped = false;

  DEF_INTERFACE(Command, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->Launch(t); }
  DEF_END(run);

  DEF_INTERFACE(Command, LongRunning, running, "Running")
  void OnCancel() { obj->Terminate(); }
  DEF_END(running);

  DEF_INTERFACE(Command, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(Command, StreamArgument, out_stream, "stdout")
  Str OnFormat() { return "bytes"; }
  StreamStats OnStats() { return obj->StdoutStats(); }
  DEF_END(out_stream);

  DEF_INTERFACE(Command, StreamInput, in_stream, "stdin")
  Str OnFormat() { return "bytes"; }
  DEF_END(in_stream);

  INTERFACES(run, running, next, out_stream, in_stream);

  Command() = default;
  Command(const Command& o)
      : Object(o), argv(o.argv), run(o.run), next(o.next), out_stream(o.out_stream) {}

  ~Command() override;

  StrView Name() const override { return "Command"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Command, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  std::string GetText() const override;          // argv joined with single spaces
  void SetText(std::string_view text) override;  // split on spaces (lossy by design)

  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  // Spawns the child and adopts `task` for the process lifetime (LongRunning).
  // On failure reports the error and leaves `task` alone.
  void Launch(std::unique_ptr<RunTask>& task);
  // Spawns this Command's argv on behalf of a restored window and takes full
  // ownership of the child (running state, STOP, exit chip), synthesizing the
  // RunTask the way Timer does when it resumes from a save. Returns the pid,
  // or 0 with `status` filled (e.g. when a child is already running).
  I64 AdoptiveLaunch(Status& status);
  // Asks the live child to exit (SIGTERM). The reaper thread completes the run.
  void Terminate();

  // The stdout stream's meters, read on every UI tick: byte totals from
  // /proc/<pid>/io wchar; pipe fill and capacity through a transient
  // pidfd_getfd of the child's fd 1 (FIONREAD, F_GETPIPE_SZ); the blocked
  // side from /proc/<pid>/syscall of this child (write on fd 1) and of the
  // downstream peer's child (read on fd 0).
  StreamStats StdoutStats();

 private:
  // Spawns this Command with the given stdio and begins the long-running
  // state. `task` may be null; a RunTask is synthesized then, the way
  // AdoptiveLaunch does. On failure returns false with `status` filled and
  // leaves `task` alone.
  bool SpawnStage(const StdioFds&, std::unique_ptr<RunTask>& task, Status& status);
};

}  // namespace automat::library
