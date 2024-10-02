// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "drawable.hh"

namespace automat {

struct SkDrawableWrapper : SkDrawable {
  Drawable* drawable;
  SkDrawableWrapper(Drawable* drawable);
  SkRect onGetBounds() override;
  void onDraw(SkCanvas* c) override;
};

}  // namespace automat