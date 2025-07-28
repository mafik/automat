// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "base.hh"
#include "gui_button.hh"
#include "widget.hh"

namespace automat::library {

struct FlipFlop;

struct YingYangIcon : gui::Widget, gui::PaintMixin {
  YingYangIcon(gui::Widget& parent) : gui::Widget(parent) {}
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  bool CenteredAtZero() const override { return true; }
};

struct FlipFlopButton : gui::ToggleButton {
  FlipFlop* flip_flop;

  FlipFlopButton(gui::Widget& parent);
  bool Filled() const override;
};

struct FlipFlop : LiveObject, Object::FallbackWidget, Runnable {
  std::unique_ptr<FlipFlopButton> button;

  bool current_state = false;
  struct AnimationState {
    float light = 0;
  };
  AnimationState animation_state;

  FlipFlop(gui::Widget& parent);
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  animation::Phase Tick(time::Timer& timer) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;

  void SetKey(gui::AnsiKey);

  void FillChildren(Vec<Widget*>& children) override;

  void OnRun(Location& here, RunTask&) override;
  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library