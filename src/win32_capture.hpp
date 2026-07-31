#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Frames of one window, as the desktop compositor already draws them
// (Windows.Graphics.Capture). They stay on the graphics card: each frame is
// copied into a shared texture that Skia samples directly.

#include <include/core/SkImage.h>
#include <include/core/SkSize.h>

#include <functional>
#include <memory>

#include "os_window.hpp"
#include "status.hpp"

namespace automat::win32_wm {

struct Capture {
  virtual ~Capture() = default;
  virtual void Pause(bool) = 0;
};

std::unique_ptr<Capture> StartCapture(os::WindowHandle,
                                      std::function<void(sk_sp<SkImage>, SkISize)> on_frame,
                                      Status&);

void ReleaseCaptureDevice();

}  // namespace automat::win32_wm
