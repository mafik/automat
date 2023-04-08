#pragma once

#include "base.h"

namespace automaton::library {

struct Increment : Object {
  static const Increment proto;
  static Argument target_arg;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Run(Location &h) override;
};

} // namespace automaton::library