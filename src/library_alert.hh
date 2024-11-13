// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "object.hh"

namespace automat {

struct Alert : Object, Runnable {
  static std::shared_ptr<Alert> proto;
  static Argument message_arg;
  std::unique_ptr<vector<string>> test_interceptor;
  string_view Name() const override { return "Alert"; }
  std::shared_ptr<Object> Clone() const override { return std::make_shared<Alert>(); }
  void Args(std::function<void(Argument&)> cb) override { cb(message_arg); }
  LongRunning* OnRun(Location& here) override;
};

}  // namespace automat