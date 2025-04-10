// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "gui_small_buffer_widget.hh"

#include <include/core/SkPath.h>

namespace automat::gui {

SmallBufferWidget::SmallBufferWidget(NestedWeakPtr<Buffer> buffer) : buffer(buffer) {
  width = 1_cm;
}

void SmallBufferWidget::Draw(SkCanvas& canvas) const { canvas.drawPath(Shape(), SkPaint()); }

RRect SmallBufferWidget::CoarseBounds() const {
  return RRect::MakeSimple(Rect::MakeAtZero<::LeftX, ::BottomY>(width, kHeight), kCornerRadius);
}

SkPath SmallBufferWidget::Shape() const { return SkPath::RRect(CoarseBounds().sk); }

}  // namespace automat::gui