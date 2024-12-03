// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "widget.hh"

namespace automat::gui {

struct AlignCenter : Widget {
  std::shared_ptr<Widget> child;

  AlignCenter(std::shared_ptr<Widget>&& child);
  SkPath Shape() const override;
  void FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) override;
  SkMatrix TransformToChild(const Widget& child) const override;
};

std::shared_ptr<Widget> MakeAlignCenter(std::shared_ptr<Widget>&& child);

}  // namespace automat::gui