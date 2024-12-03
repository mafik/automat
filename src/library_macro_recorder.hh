// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "animation.hh"
#include "base.hh"
#include "color.hh"
#include "keyboard.hh"
#include "on_off.hh"
#include "run_button.hh"

namespace automat::library {

struct GlassRunButton : gui::PowerButton {
  GlassRunButton(OnOff* on_off) : gui::PowerButton(on_off, color::kParrotRed, "#eeeeee"_color) {}
  void PointerOver(gui::Pointer&) override;
  void PointerLeave(gui::Pointer&) override;
  maf::StrView Name() const override { return "GlassRunButton"; }
};

struct MacroRecorder : LiveObject,
                       Runnable,
                       LongRunning,
                       gui::Keylogger,
                       OnOff,
                       gui::PointerMoveCallback {
  static std::shared_ptr<MacroRecorder> proto;

  struct AnimationState {
    animation::SpringV2<Vec2> googly_left;
    animation::SpringV2<Vec2> googly_right;
    float eye_rotation_speed = 0;
    float eye_rotation = 0;
    int pointers_over = 0;
    float eyes_open = 0;
  };

  mutable AnimationState animation_state;
  gui::Keylogging* keylogging = nullptr;
  std::shared_ptr<Widget> record_button;

  MacroRecorder();
  ~MacroRecorder();
  string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;
  animation::Phase Update(time::Timer& timer) override;
  animation::Phase Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  void FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) override {
    children.push_back(record_button);
  }

  void Args(std::function<void(Argument&)> cb) override;
  std::shared_ptr<Object> ArgPrototype(const Argument&) override;

  bool IsOn() const override;
  void On() override;
  void Off() override;

  void PointerOver(gui::Pointer&) override;
  void PointerLeave(gui::Pointer&) override;
  void PointerMove(gui::Pointer&, Vec2 position) override;

  SkMatrix TransformToChild(const Widget& child) const override;

  void ConnectionAdded(Location& here, Connection&) override;
  void ConnectionRemoved(Location& here, Connection&) override;

  LongRunning* OnRun(Location& here) override;
  void Cancel() override;
  void KeyloggerKeyDown(gui::Key) override;
  void KeyloggerKeyUp(gui::Key) override;

  Vec2AndDir ArgStart(const Argument&) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library