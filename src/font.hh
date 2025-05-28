// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>

#include <cmath>
#include <string_view>

#include "virtual_fs.hh"

namespace automat::gui {

struct Font {
  SkFont sk_font;
  float font_scale;
  float line_thickness;
  float letter_height;
  float ascent;   // distance to reserve above baseline, typically negative
  float descent;  // distance to reserve below baseline, typically positive

  static sk_sp<SkTypeface> LoadTypeface(fs::VFile& ttf_file);
  static sk_sp<SkTypeface> GetNotoSans();
  static sk_sp<SkTypeface> GetGrenzeThin();
  static sk_sp<SkTypeface> GetGrenzeLight();
  static sk_sp<SkTypeface> GetGrenzeRegular();
  static sk_sp<SkTypeface> GetGrenzeSemiBold();
  static sk_sp<SkTypeface> GetSilkscreen();
  static sk_sp<SkTypeface> GetHeavyData();
  static sk_sp<SkTypeface> GetHelsinki();
  static sk_sp<SkTypeface> MakeWeightVariation(sk_sp<SkTypeface> base, float weight);
  static std::unique_ptr<Font> MakeV2(sk_sp<SkTypeface> typeface, float letter_size);

  // TODO: If this causes performance issues, cache text shaping / SkTextBlob
  // results somehow
  void DrawText(SkCanvas& canvas, std::string_view text, const SkPaint& paint);
  float MeasureText(std::string_view text);
  float PositionFromIndex(std::string_view text, int index);
  int IndexFromPosition(std::string_view text, float x);
  // TODO: If this causes performance issues, use ICU directly rather than going
  // through SkShaper
  int PrevIndex(std::string_view text, int index);
  int NextIndex(std::string_view text, int index);
};

sk_sp<SkFontMgr> GetFontMgr();
Font& GetFont();

}  // namespace automat::gui