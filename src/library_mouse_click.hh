// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat::library {

struct MouseClick : Object, Object::FallbackWidget, Runnable {
  gui::PointerButton button;
  bool down;
  MouseClick(gui::PointerButton, bool down);
  string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;
  void OnRun(Location&) override;
  audio::Sound& NextSound() override;
};

}  // namespace automat::library