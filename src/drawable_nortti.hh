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