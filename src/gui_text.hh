// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "widget.hh"

namespace automat::gui {

struct Text : Widget, PaintMixin {
  std::string text;
  Text(std::string_view text = "");
  SkPath Shape() const override;
  void Draw(SkCanvas&) const override;
  maf::StrView Name() const override { return "Text"; }
  maf::Optional<Rect> TextureBounds() const override;
};

}  // namespace automat::gui