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

  struct CaptureImpl : Runnable {
    using Parent = Window;
    static constexpr StrView kName = "Capture"sv;
    static constexpr int Offset() { return offsetof(Window, capture); }

    void OnRun(std::unique_ptr<RunTask>&);
  };
  Runnable::Def<CaptureImpl> capture;

  struct NextImpl : NextArg {
    using Parent = Window;
    static constexpr StrView kName = "Next"sv;
    static constexpr int Offset() { return offsetof(Window, next); }
  };
  NextArg::Def<NextImpl> next;

  struct ImageImpl : ImageProvider {
    using Parent = Window;
    static constexpr StrView kName = "Captured Image"sv;
    static constexpr int Offset() { return offsetof(Window, image_provider); }

    sk_sp<SkImage> GetImage();
  };
  NO_UNIQUE_ADDRESS ImageProvider::Def<ImageImpl> image_provider;

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
  void AttachToTitle();

  void Relocate(Location* new_here) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;
};

}  // namespace automat::library
