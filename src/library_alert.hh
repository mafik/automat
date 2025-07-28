// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "object.hh"

namespace automat {

struct Alert : Object, Runnable {
  static Argument message_arg;
  std::unique_ptr<vector<string>> test_interceptor;
  string_view Name() const override { return "Alert"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Alert); }
  void Args(std::function<void(Argument&)> cb) override { cb(message_arg); }
  void OnRun(Location& here, RunTask&) override;
};

}  // namespace automat