// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "image_provider.hh"
#include "str.hh"
#include "time.hh"

namespace automat::library {

struct Window : public LiveObject, Runnable, ImageProvider {
  std::mutex mutex;
  Str title = "";
  bool run_continuously = true;
  sk_sp<SkImage> captured_image;  // Captured window image

  struct Impl;
  // Private implementation to avoid polluting header with platform-specific defines.
  std::unique_ptr<Impl> impl;

  time::T capture_time = 0;

  Window();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  Ptr<gui::Widget> MakeWidget() override;

  void Args(std::function<void(Argument&)> cb) override;
  void OnRun(Location& here, RunTask&) override;

  // Called after deserialization. Makes the window object attach its native handle to the window
  // with the current title.
  void AttachToTitle();

  void Relocate(Location* new_here) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;

  // ImageProvider interface
  sk_sp<SkImage> GetImage() override;
  ImageProvider* AsImageProvider() override;
};

}  // namespace automat::library