// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>

namespace automat {

// Interface for objects that can provide image data
struct ImageProvider {
  virtual ~ImageProvider() = default;
  virtual sk_sp<SkImage> GetImage() = 0;
};

}  // namespace automat