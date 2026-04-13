// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "image_provider.hh"
#include "on_off.hh"
#include "str.hh"
#include "time.hh"
#include "window_watch.hh"

namespace automat::library {

struct Window : public Object, ui::WindowWatcher {
  std::mutex mutex;
  Str title = "";
  bool run_continuously = true;
  bool active = false;            // Whether the tracked window is currently active (focused)
  sk_sp<SkImage> captured_image;  // Captured window image

  DEF_INTERFACE(Window, Runnable, capture, "Capture")
  void OnRun(std::unique_ptr<RunTask>&) { obj->Capture(); }
  DEF_END(capture);

  DEF_INTERFACE(Window, NextArg, next, "Next")
  DEF_END(next);

  DEF_INTERFACE(Window, ImageProvider, image_provider, "Captured Image")
  sk_sp<SkImage> GetImage() {
    auto lock = std::lock_guard(obj->mutex);
    return obj->captured_image;
  }
  DEF_END(image_provider);

  DEF_INTERFACE(Window, OnOff, on_off, "Active")
  bool IsOn() const { return obj->active; }
  void OnTurnOn() {
    LOG << "TODO: Activate the window";
    obj->active = true;
    obj->WakeToys();
  }
  void OnTurnOff() {
    LOG << "TODO: Deactivate the window";
    obj->active = false;
    obj->WakeToys();
  }
  void OnSync();
  void OnUnsync() {
    if (obj->window_watching) {
      obj->window_watching->Release();
      // window_watching is set to nullptr in WindowWatcherOnRelease
    }
  }
  DEF_END(on_off);

  struct Impl;
  // Private implementation to avoid polluting header with platform-specific defines.
  std::unique_ptr<Impl> impl;

  time::SteadyPoint capture_time = time::kZeroSteady;

  Window();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  INTERFACES(capture, next, image_provider, on_off);

  // Called after deserialization. Makes the window object attach its native handle to the window
  // with the current title.
  void Capture();
  void AttachToTitle();

  ui::WindowWatching* window_watching = nullptr;

  // WindowWatcher interface
  void WindowWatcherForegroundChanged(ui::WindowWatching&, os::WindowHandle window) override;
  void WindowWatcherOnRelease(const ui::WindowWatching&) override { window_watching = nullptr; }

  void Relocate(Location* new_here) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
