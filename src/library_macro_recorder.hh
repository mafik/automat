// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"
#include "library_timeline.hh"

namespace automat::library {

struct MacroRecorder : Object, ui::Keylogger, ui::Pointer::Logger {
  struct RunImpl : Runnable {
    using Parent = MacroRecorder;
    static constexpr StrView kName = "Run"sv;
    static constexpr int Offset() { return offsetof(MacroRecorder, runnable); }

    void OnRun(std::unique_ptr<RunTask>& run_task);
  };
  Runnable::Def<RunImpl> runnable;

  struct LongRunningImpl : LongRunning {
    using Parent = MacroRecorder;
    static constexpr StrView kName = "Running"sv;
    static constexpr int Offset() { return offsetof(MacroRecorder, long_running); }

    void OnCancel();
  };
  LongRunning::Def<LongRunningImpl> long_running;

  ui::Keylogging* keylogging = nullptr;
  ui::Pointer::Logging* pointer_logging = nullptr;
  WeakPtr<Timeline> timeline_connection;

  struct TimelineArgImpl : Argument {
    using Parent = MacroRecorder;
    static constexpr StrView kName = "Timeline"sv;
    static constexpr int Offset() { return offsetof(MacroRecorder, timeline); }

    static void Configure(Argument::Table&);
  };
  NO_UNIQUE_ADDRESS Argument::Def<TimelineArgImpl> timeline;

  MacroRecorder();
  MacroRecorder(const MacroRecorder&);
  ~MacroRecorder();
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  INTERFACES(runnable, long_running, timeline)

  void KeyloggerKeyDown(ui::Key) override;
  void KeyloggerKeyUp(ui::Key) override;
  void KeyloggerOnRelease(const ui::Keylogging&) override;

  void PointerLoggerButtonDown(ui::Pointer::Logging&, ui::PointerButton) override;
  void PointerLoggerButtonUp(ui::Pointer::Logging&, ui::PointerButton) override;
  void PointerLoggerScrollY(ui::Pointer::Logging&, float) override;
  void PointerLoggerScrollX(ui::Pointer::Logging&, float) override;
  void PointerLoggerMove(ui::Pointer::Logging&, Vec2) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
