// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"

namespace automat::library {

struct MouseMove : Object {
  string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<ui::Widget> MakeWidget(ui::Widget* parent) override;
  void OnMouseMove(Vec2);
};

}  // namespace automat::library