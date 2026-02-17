// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkImage.h>
#include <tesseract/baseapi.h>

#include "base.hh"
#include "image_provider.hh"
#include "str.hh"

namespace automat::library {

struct TesseractOCR : public Object {
  mutable std::mutex mutex;
  Str ocr_text = "";

  struct RunImpl : Runnable {
    using Parent = TesseractOCR;
    static constexpr StrView kName = "Run"sv;
    static constexpr int Offset() { return offsetof(TesseractOCR, run); }

    void OnRun(std::unique_ptr<RunTask>&);
  };
  Runnable::Def<RunImpl> run;

  struct NextImpl : NextArg {
    using Parent = TesseractOCR;
    static constexpr StrView kName = "Next"sv;
    static constexpr int Offset() { return offsetof(TesseractOCR, next); }
  };
  NextArg::Def<NextImpl> next;

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

  NestedWeakPtr<ImageProvider::Table> image_provider_weak;
  WeakPtr<Object> text_weak;

  struct ImageArgImpl : Argument {
    using Parent = TesseractOCR;
    static constexpr StrView kName = "Image"sv;
    static constexpr int Offset() { return offsetof(TesseractOCR, image); }

    static void Configure(Argument::Table&);
  };
  [[no_unique_address]] Argument::Def<ImageArgImpl> image;

  struct TextArgImpl : Argument {
    using Parent = TesseractOCR;
    static constexpr StrView kName = "Text"sv;
    static constexpr int Offset() { return offsetof(TesseractOCR, text); }

    static void Configure(Argument::Table&);
  };
  [[no_unique_address]] Argument::Def<TextArgImpl> text;

  TesseractOCR();

  std::string_view Name() const override;
  Ptr<Object> Clone() const override;
  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override;

  INTERFACES(image, text, next, run)
  void Updated(WeakPtr<Object>& updated) override;

  void SerializeState(ObjectSerializer& writer) const override;
  bool DeserializeKey(ObjectDeserializer& d, StrView key) override;

  std::string GetText() const override;
  void SetText(std::string_view text) override;
};

}  // namespace automat::library
