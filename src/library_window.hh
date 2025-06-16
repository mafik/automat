// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>
#include <tesseract/baseapi.h>

#include "base.hh"
#include "str.hh"

namespace automat::library {

struct Window : public LiveObject, Runnable {
  std::mutex mutex;
  Str title = "";
  Str ocr_text = "";
  bool run_continuously = true;
  sk_sp<SkImage> captured_image;  // Captured window image

  struct Impl;
  // Private implementation to avoid polluting header with platform-specific defines.
  std::unique_ptr<Impl> impl;

  tesseract::TessBaseAPI tesseract;

  float x_min_ratio = 0.25f;
  float x_max_ratio = 0.75f;
  float y_min_ratio = 0.25f;
  float y_max_ratio = 0.75f;

  Window();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  Ptr<gui::Widget> MakeWidget() override;

  // Run OCR on the currently captured window
  std::string RunOCR();

  void Args(std::function<void(Argument&)> cb) override;
  void OnRun(Location& here) override;

  // Called after deserialization. Makes the window object attach its native handle to the window
  // with the current title.
  void AttachToTitle();

  void Relocate(Location* new_here) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;
};

}  // namespace automat::library