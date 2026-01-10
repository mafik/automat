#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "on_off.hh"

namespace automat {

void FlipFlopIcon::onDraw(SkCanvas* canvas) { canvas->drawCircle(0, 0, 1_mm, paint); }

PaintDrawable& OnOff::Icon() { return icon; }

}  // namespace automat
