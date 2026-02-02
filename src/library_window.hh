// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "image_provider.hh"
#include "str.hh"
#include "time.hh"

namespace automat::library {

struct Window : public Object, ImageProvider {
  std::mutex mutex;
  Str title = "";
  bool run_continuously = true;
  sk_sp<SkImage> captured_image;  // Captured window image

  struct Capture : Runnable {
    StrView Name() const override { return "Capture"sv; }

    void OnRun(std::unique_ptr<RunTask>&) override;

    Window& GetWindow() const {
      return *reinterpret_cast<Window*>(reinterpret_cast<intptr_t>(this) -
                                        offsetof(Window, capture));
    }
  } capture;

  struct Impl;
  // Private implementation to avoid polluting header with platform-specific defines.
  std::unique_ptr<Impl> impl;

  double capture_time = 0;

  Window();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent, ReferenceCounted&) override;

  void Parts(const std::function<void(Part&)>& cb) override;

  // Called after deserialization. Makes the window object attach its native handle to the window
  // with the current title.
  void AttachToTitle();

  void Relocate(Location* new_here) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  // ImageProvider interface
  sk_sp<SkImage> GetImage() override;
  ImageProvider* AsImageProvider() override;
};

}  // namespace automat::library
