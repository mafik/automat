// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat::library {

struct Increment : Object, Runnable {
  static const Increment proto;
  static Argument target_arg;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Args(std::function<void(Argument&)> cb) override { cb(target_arg); }
  LongRunning* OnRun(Location& h) override;
  animation::Phase Draw(gui::DrawContext&) const override;
  SkPath Shape(animation::Display*) const override;
};

}  // namespace automat::library