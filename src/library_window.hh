// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "image_provider.hh"
#include "str.hh"

namespace automat::library {

struct Window : public Object {
  std::mutex mutex;
  Str title = "";
  bool run_continuously = true;
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

  struct Impl;
  // Private implementation to avoid polluting header with platform-specific defines.
  std::unique_ptr<Impl> impl;

  double capture_time = 0;

  Window();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  INTERFACES(capture, next, image_provider);

  // Called after deserialization. Makes the window object attach its native handle to the window
  // with the current title.
  void Capture();
  void AttachToTitle();

  void Relocate(Location* new_here) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
