// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>

#include "atom.hh"

namespace automat {

// Interface for objects that can provide image data
struct ImageProvider : Atom {
  virtual sk_sp<SkImage> GetImage() = 0;
};

}  // namespace automat
