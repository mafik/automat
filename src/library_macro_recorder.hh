// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"
#include "library_timeline.hh"

namespace automat::library {

struct MacroRecorder : Object, ui::Keylogger, ui::Pointer::Logger {
  std::unique_ptr<RunTask> long_running_task;
  SyncState runnable_sync;
  SyncState long_running_sync;
  static Runnable runnable;
  static LongRunning long_running;

  ui::Keylogging* keylogging = nullptr;
  ui::Pointer::Logging* pointer_logging = nullptr;
  WeakPtr<Timeline> timeline_connection;

  MacroRecorder();
  MacroRecorder(const MacroRecorder&);
  ~MacroRecorder();
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void Interfaces(const std::function<LoopControl(Interface&)>& cb) override;

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
