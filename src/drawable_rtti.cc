// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "drawable_rtti.hh"

#pragma maf add compile argument "-fno-rtti"

namespace automat {

struct SkDrawableNoRTTI : SkDrawable {
  std::unique_ptr<SkDrawableRTTI> drawable;
  SkDrawableNoRTTI(std::unique_ptr<SkDrawableRTTI> drawable) : drawable(std::move(drawable)) {}
  SkRect onGetBounds() override { return drawable->onGetBounds(); }
  void onDraw(SkCanvas* c) override { drawable->onDraw(c); }
  const char* getTypeName() const override { return drawable->getTypeName(); }
  void flatten(SkWriteBuffer& buffer) const override { drawable->flatten(buffer); }
};

sk_sp<SkDrawable> SkDrawableRTTI::Wrap(std::unique_ptr<SkDrawableRTTI> drawable) {
  return sk_sp<SkDrawable>(new SkDrawableNoRTTI(std::move(drawable)));
}

}  // namespace automat