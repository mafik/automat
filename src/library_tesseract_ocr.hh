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

  DEF_INTERFACE(TesseractOCR, Runnable, run, "Run")
  void OnRun(std::unique_ptr<RunTask>&) { obj->Run(); }
  DEF_END(run);

  DEF_INTERFACE(TesseractOCR, NextArg, next, "Next")
  DEF_END(next);

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

  DEF_INTERFACE(TesseractOCR, Argument, image, "Image")
  static constexpr auto kStyle = Argument::Style::Invisible;
  static constexpr float kAutoconnectRadius = 20_cm;
  void OnCanConnect(Interface end, Status& status);
  void OnConnect(Interface end);
  NestedPtr<Interface::Table> OnFind();
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* parent);
  DEF_END(image);

  DEF_INTERFACE(TesseractOCR, Argument, text, "Text")
  void OnCanConnect(Interface end, Status& status);
  void OnConnect(Interface end);
  NestedPtr<Interface::Table> OnFind();
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* parent);
  DEF_END(text);

  TesseractOCR();

  void Run();
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
