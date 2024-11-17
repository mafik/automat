// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "widget.hh"

namespace automat::gui {

struct AlignCenter : Widget {
  std::unique_ptr<Widget> child;

  AlignCenter(std::unique_ptr<Widget>&& child);
  SkPath Shape() const override;
  ControlFlow VisitChildren(Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child) const override;
};

std::unique_ptr<Widget> MakeAlignCenter(std::unique_ptr<Widget>&& child);

}  // namespace automat::gui