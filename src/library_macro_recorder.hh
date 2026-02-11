// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"
#include "library_timeline.hh"

namespace automat::library {

struct MacroRecorder : Object, ui::Keylogger, ui::Pointer::Logger {
  struct MyRunnable : Runnable {
    void OnRun(std::unique_ptr<RunTask>&) override;
    PARENT_REF(MacroRecorder, runnable)
  } runnable;
  struct MyLongRunning : LongRunning {
    Object* OnFindRunnable() override { return &MacroRecorder(); }
    void OnCancel() override;
    ~MyLongRunning() { OnLongRunningDestruct(); }
    PARENT_REF(MacroRecorder, long_running)
  } long_running;

  ui::Keylogging* keylogging = nullptr;
  ui::Pointer::Logging* pointer_logging = nullptr;
  NestedWeakPtr<Timeline> timeline_connection;

  MacroRecorder();
  MacroRecorder(const MacroRecorder&);
  ~MacroRecorder();
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void Atoms(const std::function<LoopControl(Atom&)>& cb) override;

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
