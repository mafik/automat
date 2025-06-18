// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>
#include <tesseract/baseapi.h>

#include "base.hh"
#include "str.hh"

namespace automat::library {

struct TesseractOCR : public LiveObject, Runnable {
  mutable std::mutex mutex;
  Str ocr_text = "";

  tesseract::TessBaseAPI tesseract;

  float x_min_ratio = 0.25f;
  float x_max_ratio = 0.75f;
  float y_min_ratio = 0.25f;
  float y_max_ratio = 0.75f;

  TesseractOCR();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  Ptr<gui::Widget> MakeWidget() override;

  void Args(std::function<void(Argument&)> cb) override;
  void OnRun(Location& here) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;

  std::string GetText() const override;
  void SetText(Location& error_context, std::string_view text) override;
};

}  // namespace automat::library