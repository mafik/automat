#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkCanvas.h>
#include <include/core/SkPicture.h>

#include "math.hpp"
#include "str.hpp"

namespace automat {

sk_sp<SkPicture> IconForExtension(StrView ext);

void DrawIconIn(SkCanvas&, const SkPicture& icon, const Rect& box);

Str MimeTypeForExtension(StrView ext);

}  // namespace automat
