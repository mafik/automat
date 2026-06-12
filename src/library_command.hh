#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "base.hh"
#include "status.hh"
#include "str.hh"
#include "vec.hh"

namespace automat::library {

// Splits a line into words on spaces - no quoting, no expansion. Used where
// flat text enters the argv world (SetText, legacy saved states); arguments
// that should contain spaces must arrive as whole argv elements instead.
Vec<Str> SplitWords(StrView line);

// The Command plate's footprint on the board; spawned windows are seated
// next to it.
Vec2 CommandPlateSize();

// Spawns argv[0] via posix_spawnp (PATH search) with the Wayland compositor
// environment injected: WAYLAND_DISPLAY points at Automat's socket and
// DISPLAY is dropped. Empty elements are skipped. Returns the child pid, or
// 0 with `status` describing the failure.
I64 SpawnArgv(const Vec<Str>& argv, Status& status);

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

  DEF_INTERFACE(Command, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->Launch(t); }
  DEF_END(run);

  DEF_INTERFACE(Command, LongRunning, running, "Running")
  void OnCancel() { obj->Terminate(); }
  DEF_END(running);

  DEF_INTERFACE(Command, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(run, running, next);

  Command() = default;
  Command(const Command& o) : Object(o), argv(o.argv), run(o.run), next(o.next) {}

  StrView Name() const override { return "Command"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Command, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;

  std::string GetText() const override;        // argv joined with single spaces
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
};

}  // namespace automat::library
