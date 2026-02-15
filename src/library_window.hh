// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include "base.hh"
#include "control_flow.hh"
#include "image_provider.hh"
#include "str.hh"

namespace automat::library {

struct Window : public Object {
  std::mutex mutex;
  Str title = "";
  bool run_continuously = true;
  sk_sp<SkImage> captured_image;  // Captured window image

  SyncState capture_sync;
  NextState next_state;
  static Runnable capture;
  static NextArg next;

  static ImageProvider image;

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
