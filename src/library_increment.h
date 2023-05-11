#pragma once

#include "base.h"

namespace automaton::library {

struct Increment : Object {
  static const Increment proto;
  static Argument target_arg;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Args(std::function<void(Argument &)> cb) override { cb(target_arg); }
  void Run(Location &h) override;
  void Draw(SkCanvas &canvas, animation::State &animation_state) const override;
  SkPath Shape() const override;
};

} // namespace automaton::library