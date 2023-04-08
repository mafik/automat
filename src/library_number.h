#pragma once

#include "base.h"

namespace automaton::library {

struct Number : Object {
  double value;
  Number(double x = 0);
  static const Number proto;
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  string GetText() const override;
  void SetText(Location &error_context, string_view text) override;
  void Draw(SkCanvas &canvas, animation::State &animation_state) const override;
  SkPath Shape() const override;
};

} // namespace automaton::library