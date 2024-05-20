#pragma once

#include <memory>

#include "base.hh"

namespace automat::library {

struct Timeline : LiveObject {
  static const Timeline proto;

  Timeline();
  string_view Name() const override;
  std::unique_ptr<Object> Clone() const override;
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  void Args(std::function<void(Argument&)> cb) override;
};

}  // namespace automat::library