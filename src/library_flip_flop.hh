// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "animation.hh"
#include "base.hh"
#include "ui_button.hh"
#include "widget.hh"

namespace automat::library {

struct FlipFlop;

struct YingYangIcon : ui::Widget, ui::PaintMixin {
  YingYangIcon(ui::Widget* parent) : ui::Widget(parent) {}
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  bool CenteredAtZero() const override { return true; }
};

struct FlipFlopButton : ui::ToggleButton {
  FlipFlop* flip_flop;

  FlipFlopButton(ui::Widget* parent);
  bool Filled() const override;
};

struct FlipFlop : LiveObject, Object::WidgetBase, Runnable, OnOff {
  std::unique_ptr<FlipFlopButton> button;

  bool current_state = false;
  struct AnimationState {
    float light = 0;
  };
  AnimationState animation_state;

  FlipFlop(ui::Widget* parent);
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  animation::Phase Tick(time::Timer& timer) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;
  operator OnOff*() override { return this; }

  void SetKey(ui::AnsiKey);

  void FillChildren(Vec<Widget*>& children) override;

  void OnRun(Location& here, std::unique_ptr<RunTask>&) override;
  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
  bool CenteredAtZero() const override { return true; }

  bool IsOn() const override { return current_state; }
  void OnTurnOn() override;
  void OnTurnOff() override;
};

}  // namespace automat::library
