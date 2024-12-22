// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkDrawable.h>
#include <include/core/SkPaint.h>
#include <include/core/SkRect.h>

namespace automat {

struct SkDrawableRTTI;

template <typename T>
concept SkDrawableRTTI_Subclass = std::derived_from<T, SkDrawableRTTI>;

// For efficiency and Objective-C compatibility, Skia avoids C++ RTTI. Because of that it's not
// possible to derive from Skia base classes in Automat. This class provides a workaround for that.
// It can be used as a base class to provide an SkDrawable-compatible interface which then can be
// wrapped in a custom SkDrawable adapter compiled with `-fno-rtti`.
struct SkDrawableRTTI {
  SkDrawableRTTI() = default;
  virtual ~SkDrawableRTTI() = default;

  virtual SkRect onGetBounds() = 0;
  virtual void onDraw(SkCanvas*) = 0;
  virtual const char* getTypeName() const = 0;
  virtual void flatten(SkWriteBuffer&) const = 0;

  // Create an instance of T (derived from SkDrawableRTTI), wrap it in a sk_sp<SkDrawable> and
  // return it. Optionally store a pointer to the typed instance in typed_ptr. Once
  // sk_sp<SkDrawable> is destroyed, the *typed_ptr becomes invalid.
  template <SkDrawableRTTI_Subclass T, typename... Args>
  static sk_sp<SkDrawable> Make(T** typed_ptr = nullptr, Args&&... args) {
    T* ptr = new T(std::forward<Args>(args)...);
    if (typed_ptr) {
      *typed_ptr = ptr;
    }
    return Wrap(std::unique_ptr<SkDrawableRTTI>(ptr));
  }

 private:
  static sk_sp<SkDrawable> Wrap(std::unique_ptr<SkDrawableRTTI> drawable);
};

}  // namespace automat