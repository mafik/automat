// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "ui_shape_widget.hh"

namespace automat::ui {

ShapeWidget::ShapeWidget(ui::Widget* parent, SkPath path) : Widget(parent), path(path) {}

SkPath ShapeWidget::Shape() const { return path; }

void ShapeWidget::Draw(SkCanvas& canvas) const { canvas.drawPath(path, paint); }

std::unique_ptr<Widget> MakeShapeWidget(ui::Widget* parent, SkPath path, SkColor fill_color) {
  auto ret = std::make_unique<ShapeWidget>(parent, path);
  ret->paint.setAntiAlias(true);
  ret->paint.setColor(fill_color);
  return ret;
}

}  // namespace automat::ui