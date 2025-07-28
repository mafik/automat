// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "gui_text.hh"

#include "font.hh"
#include "gui_constants.hh"

using namespace std;

namespace automat::gui {

Text::Text(Widget& parent, string_view text) : Widget(parent), text(text){};

SkPath Text::Shape() const {
  float w = GetFont().MeasureText(text);
  return SkPath::Rect(SkRect::MakeWH(w, kLetterSize));
}

void Text::Draw(SkCanvas& canvas) const { GetFont().DrawText(canvas, text, paint); }

Optional<Rect> Text::TextureBounds() const { return nullopt; }
}  // namespace automat::gui