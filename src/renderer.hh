// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>

namespace automat {

extern std::string debug_render_events;

// Call this first, to render the picture to the given canvas.
//
// Right after this call, make sure to present the canvas to the screen.
//
// Internally this function manages the frame packing only renders what's possible to fit in the
// 16ms frame time.
void RenderFrame(SkCanvas& canvas);

// Call this after presenting the frame.
//
// This function renders as much objects as possible to fit within the 16ms frame time.
void RenderOverflow(SkCanvas& canvas);

}  // namespace automat
