#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkPoint.h>

#include <atomic>

#include "launcher.hpp"
#include "os_window.hpp"
#include "status.hpp"

namespace automat::win32_wm {
struct Capture;
}  // namespace automat::win32_wm

namespace automat::library {

struct AppWindow : ClientWindow {
  enum class Mode : uint8_t { Embedded, Connected };
  std::atomic<Mode> mode{Mode::Embedded};
  void* prev_owner = nullptr;
  os::WindowHandle prev_hwnd = os::kNoWindow;

  std::atomic<os::WindowHandle> hwnd{os::kNoWindow};
  std::unique_ptr<win32_wm::Capture> capture;

  struct PopupMirror {
    os::WindowHandle hwnd = os::kNoWindow;
    std::unique_ptr<win32_wm::Capture> capture;
    sk_sp<SkImage> image;
    SkISize size = {};
    SkIPoint screen_pos = {};
  };
  Vec<PopupMirror> popups;

  AppWindow();
  AppWindow(const AppWindow&);
  ~AppWindow() override;

  void DecorationPreferenceChanged() override;

  StrView Name() const override { return "App Window"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

}  // namespace automat::library

namespace automat::win32_wm {

void Start(Status&);
void Stop();
void Tick();

// Asks every window of `launch` to close; with `keep_connected`, connected
// windows are left running. Returns false when the launch has no windows.
bool CloseWindowsOf(Launch&, bool keep_connected);

}  // namespace automat::win32_wm
