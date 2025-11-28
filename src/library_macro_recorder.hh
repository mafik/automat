// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "base.hh"
#include "color.hh"
#include "keyboard.hh"
#include "on_off.hh"
#include "run_button.hh"

namespace automat::library {

struct GlassRunButton : ui::PowerButton {
  GlassRunButton(ui::Widget* parent, OnOff* on_off)
      : ui::PowerButton(parent, on_off, color::kParrotRed, "#eeeeee"_color) {}
  void PointerOver(ui::Pointer&) override;
  void PointerLeave(ui::Pointer&) override;
  StrView Name() const override { return "GlassRunButton"; }
};

struct MacroRecorder : LiveObject,
                       Object::WidgetBase,
                       Runnable,
                       LongRunning,
                       ui::Keylogger,
                       ui::Pointer::Logger,
                       ui::PointerMoveCallback {
  struct AnimationState {
    animation::SpringV2<Vec2> googly_left;
    animation::SpringV2<Vec2> googly_right;
    float eye_rotation_speed = 0;
    float eye_rotation = 0;
    int pointers_over = 0;
    float eyes_open = 0;
  };

  mutable AnimationState animation_state;
  ui::Keylogging* keylogging = nullptr;
  ui::Pointer::Logging* pointer_logging = nullptr;
  std::unique_ptr<ui::Widget> record_button;

  MacroRecorder(ui::Widget* parent);
  ~MacroRecorder();
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  animation::Phase Tick(time::Timer& timer) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void FillChildren(Vec<Widget*>& children) override { children.push_back(record_button.get()); }

  void Args(std::function<void(Argument&)> cb) override;
  Ptr<Object> ArgPrototype(const Argument&) override;

  void PointerOver(ui::Pointer&) override;
  void PointerLeave(ui::Pointer&) override;
  void PointerMove(ui::Pointer&, Vec2 position) override;

  void ConnectionAdded(Location& here, Connection&) override;
  void ConnectionRemoved(Location& here, Connection&) override;

  void OnRun(Location& here, std::unique_ptr<RunTask>&) override;
  void OnCancel() override;
  LongRunning* AsLongRunning() override { return this; }
  void KeyloggerKeyDown(ui::Key) override;
  void KeyloggerKeyUp(ui::Key) override;

  void PointerLoggerButtonDown(ui::Pointer::Logging&, ui::PointerButton) override;
  void PointerLoggerButtonUp(ui::Pointer::Logging&, ui::PointerButton) override;
  void PointerLoggerScrollY(ui::Pointer::Logging&, float) override;
  void PointerLoggerScrollX(ui::Pointer::Logging&, float) override;
  void PointerLoggerMove(ui::Pointer::Logging&, Vec2) override;

  Vec2AndDir ArgStart(const Argument&) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library
