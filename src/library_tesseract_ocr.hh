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

  // Guards access to the status variables
  std::mutex status_mutex;
  // Area which is currently being OCRed in pixels (within the OCR window)
  Rect status_rect;
  // Progress ratio of the OCR (0.0 to 1.0)
  Optional<float> status_progress_ratio = std::nullopt;

  struct RecognitionResult {
    Rect rect;  // In pixels (within the image)
    Str text;
  };

  Vec<RecognitionResult> status_results;

  tesseract::TessBaseAPI tesseract;

  float x_min_ratio = 0.25f;
  float x_max_ratio = 0.75f;
  float y_min_ratio = 0.25f;
  float y_max_ratio = 0.75f;

  TesseractOCR();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<WidgetInterface> MakeWidget(ui::Widget* parent) override;

  void Args(std::function<void(Argument&)> cb) override;
  void Updated(Location& here, Location& updated) override;
  void OnRun(Location& here, std::unique_ptr<RunTask>&) override;

  void SerializeState(Serializer& writer, const char* key) const override;
  void DeserializeState(Location& l, Deserializer& d) override;

  std::string GetText() const override;
  void SetText(Location& error_context, std::string_view text) override;
};

}  // namespace automat::library
