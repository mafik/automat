// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat::library {

struct Increment : Object, gui::Widget, Runnable {
  static Argument target_arg;
  string_view Name() const override;
  std::shared_ptr<Object> Clone() const override;
  void Args(std::function<void(Argument&)> cb) override { cb(target_arg); }
  LongRunning* OnRun(Location& h) override;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
};

}  // namespace automat::library