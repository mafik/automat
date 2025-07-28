// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat::library {

struct Increment : Object, gui::Widget, Runnable {
  static Argument target_arg;
  Increment(Widget& parent) : gui::Widget(parent) {}
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  void Args(std::function<void(Argument&)> cb) override { cb(target_arg); }
  void OnRun(Location& h, RunTask&) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
};

}  // namespace automat::library