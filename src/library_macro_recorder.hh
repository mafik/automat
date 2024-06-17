#pragma once

#include "base.hh"

namespace automat::library {

struct MacroRecorder : Object, Runnable, LongRunning {
  static const MacroRecorder proto;

  MacroRecorder();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;

  LongRunning* OnRun(Location& here) override;
  void Cancel() override;
};

}  // namespace automat::library