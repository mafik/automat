// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "keyboard.hh"
#include "library_timeline.hh"

namespace automat::library {

struct MacroRecorder : Object, ui::Keylogger, ui::Pointer::Logger {
  DEF_INTERFACE(MacroRecorder, Runnable, runnable, "Run")
  void OnRun(std::unique_ptr<RunTask>& run_task) { obj->StartRecording(run_task); }
  DEF_END(runnable);

  DEF_INTERFACE(MacroRecorder, LongRunning, long_running, "Running")
  void OnCancel() { obj->StopRecording(); }
  DEF_END(long_running);

  ui::Keylogging* keylogging = nullptr;
  ui::Pointer::Logging* pointer_logging = nullptr;

  DEF_INTERFACE(MacroRecorder, ObjectArgument<Timeline>, timeline, "Timeline")
  static constexpr auto kStyle = Argument::Style::Cable;
  static constexpr float kAutoconnectRadius = 10_cm;
  static constexpr SkColor kTint = color::kParrotRed;
  static Ptr<Object> MakePrototype();
  void OnConnect(Interface end);
  DEF_END(timeline);

  MacroRecorder();
  MacroRecorder(const MacroRecorder&);
  ~MacroRecorder();
  void StartRecording(std::unique_ptr<RunTask>&);
  void StopRecording();
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
