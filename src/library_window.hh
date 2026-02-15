// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "control_flow.hh"
#include "image_provider.hh"
#include "parent_ref.hh"
#include "str.hh"

namespace automat::library {

struct Window : public Object {
  std::mutex mutex;
  Str title = "";
  bool run_continuously = true;
  sk_sp<SkImage> captured_image;  // Captured window image

  struct Capture : Runnable {
    StrView Name() const override { return "Capture"sv; }

    void OnRun(std::unique_ptr<RunTask>&) override;

    PARENT_REF(Window, capture)
  } capture;

  struct CapturedImage : ImageProvider {
    StrView Name() const override { return "Captured Image"sv; }
    sk_sp<SkImage> GetImage() override {
      auto& w = Window();
      auto lock = std::lock_guard(w.mutex);
      return w.captured_image;
    }
    PARENT_REF(Window, image);
  } image;

  struct Impl;
  // Private implementation to avoid polluting header with platform-specific defines.
  std::unique_ptr<Impl> impl;

  double capture_time = 0;

  Window();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  void Interfaces(const std::function<LoopControl(Interface&)>& cb) override;

  // Called after deserialization. Makes the window object attach its native handle to the window
  // with the current title.
  void AttachToTitle();

  void Relocate(Location* new_here) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  // ImageProvider interface
  ImageProvider* AsImageProvider() override { return &image; }
};

}  // namespace automat::library
