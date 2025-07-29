// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>

#include "widget.hh"

namespace automat::ui {

struct ShapeWidget : Widget, PaintMixin {
  SkPath path;

  ShapeWidget(ui::Widget* parent, SkPath path);
  SkPath Shape() const override;
  void Draw(SkCanvas&) const override;
  bool CenteredAtZero() const override { return true; }
};

std::unique_ptr<Widget> MakeShapeWidget(ui::Widget* parent, const char* svg_path,
                                        SkColor fill_color, const SkMatrix* transform = nullptr);

}  // namespace automat::ui