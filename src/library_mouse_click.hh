// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat::library {

struct MouseClick : Object, Runnable {
  gui::PointerButton button;
  bool down;
  MouseClick(gui::PointerButton, bool down);
  static std::shared_ptr<MouseClick> lmb_down;
  static std::shared_ptr<MouseClick> lmb_up;
  static std::shared_ptr<MouseClick> rmb_down;
  static std::shared_ptr<MouseClick> rmb_up;
  string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;
  LongRunning* OnRun(Location&) override;
  audio::Sound& NextSound() override;
};

}  // namespace automat::library