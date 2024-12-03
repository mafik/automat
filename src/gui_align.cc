// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "gui_align.hh"

#include <include/core/SkMatrix.h>

namespace automat::gui {

AlignCenter::AlignCenter(std::shared_ptr<Widget>&& child) : child(std::move(child)) {}

SkPath AlignCenter::Shape() const { return SkPath(); }

void AlignCenter::FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) {
  if (child) {
    children.push_back(child);
  }
}
SkMatrix AlignCenter::TransformToChild(const Widget& child_arg) const {
  if (&child_arg != this->child.get()) {
    return SkMatrix::I();
  }
  SkRect bounds = child->Shape().getBounds();
  Vec2 c = bounds.center();
  return SkMatrix::Translate(c);
}

}  // namespace automat::gui