#pragma once

#include "base.h"
#include "object.h"

namespace automaton {

struct Alert : Object {
  static const Alert proto;
  static Argument message_arg;
  std::unique_ptr<vector<string>> test_interceptor;
  string_view Name() const override { return "Alert"; }
  std::unique_ptr<Object> Clone() const override {
    return std::make_unique<Alert>();
  }
  void Run(Location &here) override;
};

} // namespace automaton