#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "base.hpp"
#include "launcher.hpp"
#include "status.hpp"
#include "str.hpp"
#include "stream.hpp"
#include "vec.hpp"

namespace automat::library {

// Splits a line into words on spaces - no quoting, no expansion. Used where
// flat text enters the argv world (SetText, legacy saved states); arguments
// that should contain spaces must arrive as whole argv elements instead.
Vec<Str> SplitWords(StrView line);

// Command holds one program invocation - an executable name plus arguments -
// and runs it as a child process. There is no shell anywhere: `argv` reaches
// posix_spawnp exactly as stored, so quoting, globs and $variables do not
// exist. Elements may contain spaces (one element = one argument, always);
// empty elements are transient editor state, skipped at spawn and save.
struct Command : Object, Container {
  mutable std::mutex mutex;  // guards argv and the launch below
  Vec<Str> argv;             // argv[0] is the program
  Ptr<Launch> launch;        // current or last run; replaced by the next run
  bool ever_ran = false;

  DEF_INTERFACE(Command, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>& t) { obj->Run(t); }
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

  // Runs the connected stdio chain, spawning every idle downstream stage the
  // way a shell starts a pipeline. On failure reports the error and leaves
  // `task` alone.
  void Run(std::unique_ptr<RunTask>& task);
  // Launches this Command's argv with the launch aimed at the given window,
  // synthesizing the RunTask the way Timer does when it resumes from a save.
  // Returns the launch, or null with `status` filled.
  Ptr<Launch> RunFor(ClientWindow& window, Status& status);
  // Asks the live child to exit (SIGTERM).
  void Terminate();
  // Takes the launch out of this Command, leaving it free to run again. The
  // running state ends without cancelling the child or scheduling `next`.
  Ptr<Launch> ExtractLaunch();
  Container* AsContainer() override { return this; }
  Ptr<Location> Extract(Object& descendant) override;

  StreamStats StdoutStats();

 private:
  bool Busy();
  // Spawns one stage and begins the long-running state. `task` may be null; a
  // RunTask is synthesized then. On failure returns false with `status`
  // filled and leaves `task` alone.
  bool SpawnStage(const SpawnFds&, ClientWindow* restoring, std::unique_ptr<RunTask>& task,
                  Status& status);
};

}  // namespace automat::library
