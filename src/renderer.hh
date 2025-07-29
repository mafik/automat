// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>

#include <vector>

namespace automat {

namespace ui {
struct Widget;
}

struct RenderResult {
  uint32_t id;
  float render_time;
};

struct PackFrameRequest {
  // Must be sorted by ID!
  std::vector<RenderResult> render_results;
};

// Create a SkDrawable that will draw the given widget. The drawable may cache the rendered widget
// as a texture to speed up subsequent re-renders.
sk_sp<SkDrawable> MakeWidgetDrawable(ui::Widget& widget);

extern PackFrameRequest next_frame_request;

void RendererInit();

// Releases resources used by the renderer.
void RendererShutdown();

// Call this first, to render the picture to the given canvas.
//
// Internally this function manages the frame packing only renders what's possible to fit in the
// 16ms frame time.
void RenderFrame(SkCanvas& canvas);

}  // namespace automat
