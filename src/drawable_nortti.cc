// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "drawable_nortti.hh"

#pragma maf add compile argument "-fno-rtti"

namespace automat {
SkDrawableWrapper::SkDrawableWrapper(Drawable* drawable) : drawable(drawable) {}
SkRect SkDrawableWrapper::onGetBounds() { return drawable->onGetBounds(); }
void SkDrawableWrapper::onDraw(SkCanvas* c) { drawable->onDraw(c); }
}  // namespace automat