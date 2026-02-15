// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>

#include "interface.hh"

namespace automat {

struct Object;

// Interface for objects that can provide image data
struct ImageProvider : Interface {
  static bool classof(const Interface* i) { return i->kind == Interface::kImageProvider; }

  // Function pointer for getting the image.
  sk_sp<SkImage> (*get_image)(const ImageProvider&, Object&) = nullptr;

  ImageProvider(StrView name) : Interface(Interface::kImageProvider, name) {}

  template <typename T>
  ImageProvider(StrView name, sk_sp<SkImage> (*get_image_fn)(const ImageProvider&, T&))
      : ImageProvider(name) {
    get_image = reinterpret_cast<sk_sp<SkImage> (*)(const ImageProvider&, Object&)>(get_image_fn);
  }

  sk_sp<SkImage> GetImage(Object& self) const {
    return get_image ? get_image(*this, self) : nullptr;
  }
};

}  // namespace automat
