// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "theme_xp.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/effects/SkGradientShader.h>

#include <algorithm>

#include "color.hh"
#include "math.hh"

namespace automat::theme::xp {

sk_sp<SkVertices> WindowBorder(Rect outer, SkColor title_color) {
  constexpr SkColor kFillColor = "#ece9d8"_color;
  // constexpr SkColor kBrightColor = "#3399ff"_color;
  // constexpr SkColor kMainColor = "#0066ff"_color;  // a beautiful electric blue
  // constexpr SkColor kDarkColor = "#0033cc"_color;
  // constexpr SkColor kDarkerColor = "#003399"_color;

  Vec3 hsluv = color::ToHSLuv(title_color);

  // Create darker and brighter variants for the borders
  SkColor title_dark = color::AdjustLightness(title_color, -15);
  SkColor title_medium = color::AdjustLightness(title_color, -5);
  SkColor title_bright = color::AdjustLightness(title_color, 5);

  // Names for the vertices of the top left & top right of the window border.
  enum TopBorder { TopBorder_Outer, TopBorder_Middle, TopBorder_Inner, TopBorder_Count };

  // Names for the vertices in the bottom corners of the window border.
  enum BottomCorner {
    BottomCorner_TopOuter,
    BottomCorner_TopMiddle,
    BottomCorner_TopInner,
    BottomCorner_Center,
    BottomCorner_BottomOuter,
    BottomCorner_Count
  };

  constexpr int kTitleGridColumns = 16;
  constexpr int kTitleGridRows = 8;
  constexpr int kTitleGridCornerCells = 3;
  constexpr int kTitleGridCornerBeams = 8;

  constexpr float kTitleGridCellSize = kTitleBarHeight / kTitleGridRows;
  constexpr float kTitleGridCornerRadius = kTitleGridCellSize * kTitleGridCornerCells;
  constexpr float kTitleGridWidth = kTitleGridCellSize * kTitleGridColumns;

  constexpr int kTitleCornerVertices = (kTitleGridCornerBeams + 1) * kTitleGridCornerCells + 1;

  constexpr int kTitleSmallGridRows = kTitleGridRows - kTitleGridCornerCells;
  constexpr int kTitleSmallGridCols = kTitleGridCornerCells;

  constexpr int kTitleSmallGridVertices = (kTitleSmallGridCols + 1) * (kTitleSmallGridRows + 1);

  constexpr int kTitleLargeGridRows = kTitleGridRows;
  constexpr int kTitleLargeGridCols = kTitleGridColumns - kTitleGridCornerCells;

  constexpr int kTitleLargeGridVertices = (kTitleLargeGridCols + 1) * (kTitleLargeGridRows + 1);

  constexpr int kNumVertices = (int)TopBorder_Count * 2 + (int)BottomCorner_Count * 2 +
                               kTitleCornerVertices * 2 + kTitleSmallGridVertices * 2 +
                               kTitleLargeGridVertices * 2 + 4 /* fill */;

  constexpr int kNumVerticalBorderTriangles = 4;
  constexpr int kNumBottomCornerTriangles = 3;
  constexpr int kNumBottomBorderTriangles = 4;

  constexpr int kNumTitleCornerTriangles =
      (kTitleGridCornerBeams) * (kTitleGridCornerCells - 1) * 2 + kTitleGridCornerBeams;

  constexpr int kNumTitleSmallGridTriangles = kTitleSmallGridRows * kTitleSmallGridCols * 2;

  constexpr int kNumTitleLargeGridTriangles = kTitleLargeGridRows * kTitleLargeGridCols * 2;

  constexpr int kNumTitleCenterTriangles = kTitleGridRows * 2;

  constexpr int kNumTriangles = kNumVerticalBorderTriangles * 2 + kNumBottomCornerTriangles * 2 +
                                kNumBottomBorderTriangles + kNumTitleCornerTriangles * 2 +
                                kNumTitleSmallGridTriangles * 2 + kNumTitleLargeGridTriangles * 2 +
                                kNumTitleCenterTriangles + 2;

  constexpr int kNumIndices = kNumTriangles * 3;

  auto builder = SkVertices::Builder(SkVertices::kTriangles_VertexMode, kNumVertices, kNumIndices,
                                     SkVertices::kHasColors_BuilderFlag);
  SkPoint* pos = builder.positions();
  SkColor* colors = builder.colors();

  const Vec2 top_left = outer.TopLeftCorner();
  const Vec2 top_right = outer.TopRightCorner();
  const Vec2 bottom_left = outer.BottomLeftCorner();
  const Vec2 bottom_right = outer.BottomRightCorner();

  constexpr float w = kBorderWidth;

  constexpr int kTopLeftBorderBase = 0;
  pos[kTopLeftBorderBase + TopBorder_Outer] = top_left + Vec2(0, -kTitleBarHeight);
  pos[kTopLeftBorderBase + TopBorder_Middle] = top_left + Vec2(w / 2, -kTitleBarHeight);
  pos[kTopLeftBorderBase + TopBorder_Inner] = top_left + Vec2(w, -kTitleBarHeight);

  colors[kTopLeftBorderBase + TopBorder_Outer] = title_dark;
  colors[kTopLeftBorderBase + TopBorder_Middle] = title_medium;
  colors[kTopLeftBorderBase + TopBorder_Inner] = title_color;

  constexpr int kBottomLeftBorderBase = kTopLeftBorderBase + TopBorder_Count;
  pos[kBottomLeftBorderBase + BottomCorner_TopOuter] = bottom_left + Vec2(0, w);
  pos[kBottomLeftBorderBase + BottomCorner_TopMiddle] = bottom_left + Vec2(w / 2, w);
  pos[kBottomLeftBorderBase + BottomCorner_TopInner] = bottom_left + Vec2(w, w);
  pos[kBottomLeftBorderBase + BottomCorner_Center] = bottom_left + Vec2(w / 2, w / 2);
  pos[kBottomLeftBorderBase + BottomCorner_BottomOuter] = bottom_left;
  colors[kBottomLeftBorderBase + BottomCorner_TopOuter] = title_dark;
  colors[kBottomLeftBorderBase + BottomCorner_TopMiddle] = title_medium;
  colors[kBottomLeftBorderBase + BottomCorner_TopInner] = title_color;
  colors[kBottomLeftBorderBase + BottomCorner_Center] = title_dark;
  colors[kBottomLeftBorderBase + BottomCorner_BottomOuter] = title_dark;

  constexpr int kBottomRightBorderBase = kBottomLeftBorderBase + BottomCorner_Count;
  pos[kBottomRightBorderBase + BottomCorner_TopOuter] = bottom_right + Vec2(0, w);
  pos[kBottomRightBorderBase + BottomCorner_TopMiddle] = bottom_right + Vec2(-w / 2, w);
  pos[kBottomRightBorderBase + BottomCorner_TopInner] = bottom_right + Vec2(-w, w);
  pos[kBottomRightBorderBase + BottomCorner_Center] = bottom_right + Vec2(-w / 2, w / 2);
  pos[kBottomRightBorderBase + BottomCorner_BottomOuter] = bottom_right;
  colors[kBottomRightBorderBase + BottomCorner_TopOuter] = title_dark;
  colors[kBottomRightBorderBase + BottomCorner_TopMiddle] = title_medium;
  colors[kBottomRightBorderBase + BottomCorner_TopInner] = title_color;
  colors[kBottomRightBorderBase + BottomCorner_Center] = title_dark;
  colors[kBottomRightBorderBase + BottomCorner_BottomOuter] = title_dark;

  constexpr int kTopRightBorderBase = kBottomRightBorderBase + BottomCorner_Count;
  pos[kTopRightBorderBase + TopBorder_Outer] = top_right + Vec2(0, -kTitleBarHeight);
  pos[kTopRightBorderBase + TopBorder_Middle] = top_right + Vec2(-w / 2, -kTitleBarHeight);
  pos[kTopRightBorderBase + TopBorder_Inner] = top_right + Vec2(-w, -kTitleBarHeight);
  colors[kTopRightBorderBase + TopBorder_Outer] = title_dark;
  colors[kTopRightBorderBase + TopBorder_Middle] = title_medium;
  colors[kTopRightBorderBase + TopBorder_Inner] = title_color;

  auto TitleShader = [&](SinCos edge_dir, float edge_dist, float horiz_edge_dist,
                         float vert_edge_dist) {
    // Debug view shader parameters
    // return SkColorSetRGB((float)edge_dir.sin * 255 / 100, horiz_edge_dist / 3_cm * 255,
    //                      vert_edge_dist / kTitleBarHeight * 255);

    SkColor edge_color = color::FastMix(title_dark, title_bright, (float)edge_dir.sin);
    SkColor base_color = title_color;

    // Subtle shade in the top half of the title bar
    float middle_inset =
        sinf(std::clamp(vert_edge_dist * 2 / kTitleBarHeight - 0.3f, 0.f, 1.f) * M_PI);
    middle_inset = middle_inset * 0.5f + 0.5f;
    base_color = color::FastMix(base_color, title_dark, middle_inset * 0.6f);

    // Subtle highlight on the bottom half of the title bar
    float middle_outset =
        sinf(std::clamp(vert_edge_dist * 2 / kTitleBarHeight - 0.8f, 0.f, 1.f) * M_PI);
    base_color = color::FastMix(base_color, title_bright, middle_outset * 0.4f);

    // Flat region near the horizontal edge
    float edge_flatten =
        cosf(std::clamp<float>(horiz_edge_dist / kTitleGridWidth, 0.f, 1.f) * M_PI);
    edge_flatten = edge_flatten * 0.5f + 0.5f;
    base_color = color::FastMix(base_color, title_color, edge_flatten * 0.7f);

    // Highlight / Shadow on the edges
    if (edge_dist <= 1_mm) {
      base_color = color::FastMix(edge_color, base_color, edge_dist / 1_mm);
    }
    float bottom_edge_dist = kTitleBarHeight - vert_edge_dist;
    if (bottom_edge_dist <= 1_mm) {
      base_color = color::FastMix(title_dark, base_color, bottom_edge_dist / 1_mm);
    }
    return base_color;
  };

  constexpr int kTitleLeftCornerBase = kTopRightBorderBase + TopBorder_Count;
  auto TitleLeftCornerI = [&](int beam, int cell) {
    return kTitleLeftCornerBase + cell * (kTitleGridCornerBeams + 1) + beam;
  };
  int title_left_corner_center_i =
      kTitleLeftCornerBase + (kTitleGridCornerBeams + 1) * kTitleGridCornerCells;
  for (int beam = 0; beam <= kTitleGridCornerBeams; ++beam) {
    auto angle = SinCos::FromDegrees(180 - 90.f * beam / kTitleGridCornerBeams);
    for (int cell = 0; cell < kTitleGridCornerCells; ++cell) {
      int i = TitleLeftCornerI(beam, cell);
      float length = kTitleGridCornerRadius * (cell + 1) / kTitleGridCornerCells;
      Vec2 delta = Vec2::Polar(angle, length);
      pos[i] = top_left + Vec2(kTitleGridCornerRadius, -kTitleGridCornerRadius) + delta;
      colors[i] = TitleShader(angle, kTitleGridCornerRadius - length,
                              kTitleGridCornerRadius + delta.x, kTitleGridCornerRadius - delta.y);
    }
  }
  pos[title_left_corner_center_i] =
      top_left + Vec2(kTitleGridCornerRadius, -kTitleGridCornerRadius);
  colors[title_left_corner_center_i] =
      TitleShader(135_deg, kTitleGridCornerRadius, kTitleGridCornerRadius, kTitleGridCornerRadius);

  constexpr int kTitleLeftSmallGridBase = kTitleLeftCornerBase + kTitleCornerVertices;
  auto TitleLeftSmallGridI = [&](int row, int col) {
    return kTitleLeftSmallGridBase + row * (kTitleSmallGridCols + 1) + col;
  };
  for (int row = 0; row <= kTitleSmallGridRows; ++row) {
    for (int col = 0; col <= kTitleSmallGridCols; ++col) {
      int i = TitleLeftSmallGridI(row, col);
      pos[i] =
          top_left + Vec2(kTitleGridCellSize * col, -kTitleBarHeight + kTitleGridCellSize * row);
      colors[i] = TitleShader(180_deg, kTitleGridCellSize * col, kTitleGridCellSize * col,
                              kTitleBarHeight - kTitleGridCellSize * row);
    }
  }

  constexpr int kTitleLeftLargeGridBase = kTitleLeftSmallGridBase + kTitleSmallGridVertices;
  constexpr int kTitleRightLargeGridBase = kTitleLeftLargeGridBase + kTitleLargeGridVertices;
  auto TitleLeftLargeGridI = [&](int row, int col) {
    return kTitleLeftLargeGridBase + row * (kTitleLargeGridCols + 1) + col;
  };
  auto TitleRightLargeGridI = [&](int row, int col) {
    return kTitleRightLargeGridBase + row * (kTitleLargeGridCols + 1) + col;
  };
  for (int row = 0; row <= kTitleLargeGridRows; ++row) {
    for (int col = 0; col <= kTitleLargeGridCols; ++col) {
      int l = TitleLeftLargeGridI(row, col);
      int r = TitleRightLargeGridI(row, col);
      pos[l] = top_left + Vec2(kTitleGridCornerRadius + kTitleGridCellSize * col,
                               -kTitleBarHeight + kTitleGridCellSize * row);
      pos[r] = top_right + Vec2(-kTitleGridCornerRadius - kTitleGridCellSize * col,
                                -kTitleBarHeight + kTitleGridCellSize * row);
      colors[l] = colors[r] = TitleShader(90_deg, kTitleBarHeight - kTitleGridCellSize * row,
                                          kTitleGridCornerRadius + kTitleGridCellSize * col,
                                          kTitleBarHeight - kTitleGridCellSize * row);
    }
  }

  constexpr int kTitleRightCornerBase = kTitleRightLargeGridBase + kTitleLargeGridVertices;

  auto TitleRightCornerI = [&](int beam, int cell) {
    return kTitleRightCornerBase + cell * (kTitleGridCornerBeams + 1) + beam;
  };
  int title_right_corner_center_i =
      kTitleRightCornerBase + (kTitleGridCornerBeams + 1) * kTitleGridCornerCells;
  for (int beam = 0; beam <= kTitleGridCornerBeams; ++beam) {
    auto angle = SinCos::FromDegrees(90.f * beam / kTitleGridCornerBeams);
    for (int cell = 0; cell < kTitleGridCornerCells; ++cell) {
      int i = TitleRightCornerI(beam, cell);
      float length = kTitleGridCornerRadius * (cell + 1) / kTitleGridCornerCells;
      Vec2 delta = Vec2::Polar(angle, length);
      pos[i] = top_right + Vec2(-kTitleGridCornerRadius, -kTitleGridCornerRadius) + delta;
      colors[i] = TitleShader(angle, kTitleGridCornerRadius - length,
                              kTitleGridCornerRadius - delta.x, kTitleGridCornerRadius - delta.y);
    }
  }
  pos[title_right_corner_center_i] =
      top_right + Vec2(-kTitleGridCornerRadius, -kTitleGridCornerRadius);
  colors[title_right_corner_center_i] =
      TitleShader(45_deg, kTitleGridCornerRadius, kTitleGridCornerRadius, kTitleGridCornerRadius);

  constexpr int kTitleRightSmallGridBase = kTitleRightCornerBase + kTitleCornerVertices;
  auto TitleRightSmallGridI = [&](int row, int col) {
    return kTitleRightSmallGridBase + row * (kTitleSmallGridCols + 1) + col;
  };
  for (int row = 0; row <= kTitleSmallGridRows; ++row) {
    for (int col = 0; col <= kTitleSmallGridCols; ++col) {
      int i = TitleRightSmallGridI(row, col);
      pos[i] =
          top_right + Vec2(-kTitleGridCellSize * col, -kTitleBarHeight + kTitleGridCellSize * row);
      colors[i] = TitleShader(0_deg, kTitleGridCellSize * col, kTitleGridCellSize * col,
                              kTitleBarHeight - kTitleGridCellSize * row);
    }
  }

  constexpr int kFillBase = kTitleRightSmallGridBase + kTitleSmallGridVertices;
  pos[kFillBase] = pos[kTopLeftBorderBase + TopBorder_Inner];
  pos[kFillBase + 1] = pos[kTopRightBorderBase + TopBorder_Inner];
  pos[kFillBase + 2] = pos[kBottomRightBorderBase + BottomCorner_TopInner];
  pos[kFillBase + 3] = pos[kBottomLeftBorderBase + BottomCorner_TopInner];
  colors[kFillBase] = colors[kFillBase + 1] = colors[kFillBase + 2] = colors[kFillBase + 3] =
      kFillColor;

  constexpr int kIndexEnd = kFillBase + 4;

  static_assert(kIndexEnd == kNumVertices, "kIndexEnd != kNumVertices");

  uint16_t* ind = builder.indices();
  int i = 0;

  auto Triangle = [&](int a, int b, int c) {
    ind[i++] = a;
    ind[i++] = b;
    ind[i++] = c;
  };

  auto Quad = [&](int a, int b, int c, int d) {
    Triangle(a, b, c);
    Triangle(a, c, d);
  };

  auto Fan = [&](int center, std::initializer_list<int> pts) {
    auto it = pts.begin();
    if (it == pts.end()) {
      return;
    }
    ++it;
    while (it != pts.end()) {
      Triangle(center, *(it - 1), *it);
      ++it;
    }
  };

  // Left & right title corners
  for (int beam = 0; beam < kTitleGridCornerBeams; ++beam) {
    Triangle(title_left_corner_center_i, TitleLeftCornerI(beam, 0), TitleLeftCornerI(beam + 1, 0));
    Triangle(title_right_corner_center_i, TitleRightCornerI(beam, 0),
             TitleRightCornerI(beam + 1, 0));
    for (int cell = 1; cell < kTitleGridCornerCells; ++cell) {
      Quad(TitleLeftCornerI(beam, cell - 1), TitleLeftCornerI(beam, cell),
           TitleLeftCornerI(beam + 1, cell), TitleLeftCornerI(beam + 1, cell - 1));
      Quad(TitleRightCornerI(beam, cell - 1), TitleRightCornerI(beam, cell),
           TitleRightCornerI(beam + 1, cell), TitleRightCornerI(beam + 1, cell - 1));
    }
  }

  // Left & right small grids
  for (int row = 0; row < kTitleSmallGridRows; ++row) {
    for (int col = 0; col < kTitleSmallGridCols; ++col) {
      Quad(TitleLeftSmallGridI(row, col), TitleLeftSmallGridI(row, col + 1),
           TitleLeftSmallGridI(row + 1, col + 1), TitleLeftSmallGridI(row + 1, col));
      Quad(TitleRightSmallGridI(row, col), TitleRightSmallGridI(row, col + 1),
           TitleRightSmallGridI(row + 1, col + 1), TitleRightSmallGridI(row + 1, col));
    }
  }

  // Left & right large grids
  for (int row = 0; row < kTitleLargeGridRows; ++row) {
    for (int col = 0; col < kTitleLargeGridCols; ++col) {
      Quad(TitleLeftLargeGridI(row, col), TitleLeftLargeGridI(row, col + 1),
           TitleLeftLargeGridI(row + 1, col + 1), TitleLeftLargeGridI(row + 1, col));
      Quad(TitleRightLargeGridI(row, col), TitleRightLargeGridI(row, col + 1),
           TitleRightLargeGridI(row + 1, col + 1), TitleRightLargeGridI(row + 1, col));
    }
    // Connect the left & right grids
    Quad(TitleLeftLargeGridI(row, kTitleLargeGridCols),
         TitleLeftLargeGridI(row + 1, kTitleLargeGridCols),
         TitleRightLargeGridI(row + 1, kTitleLargeGridCols),
         TitleRightLargeGridI(row, kTitleLargeGridCols));
  }

  // Left border
  Quad(kTopLeftBorderBase + TopBorder_Outer, kTopLeftBorderBase + TopBorder_Middle,
       kBottomLeftBorderBase + BottomCorner_TopMiddle,
       kBottomLeftBorderBase + BottomCorner_TopOuter);
  Quad(kTopLeftBorderBase + TopBorder_Middle, kTopLeftBorderBase + TopBorder_Inner,
       kBottomLeftBorderBase + BottomCorner_TopInner,
       kBottomLeftBorderBase + BottomCorner_TopMiddle);

  // Bottom left corner
  Fan(kBottomLeftBorderBase + BottomCorner_Center,
      {kBottomLeftBorderBase + BottomCorner_BottomOuter,
       kBottomLeftBorderBase + BottomCorner_TopOuter,
       kBottomLeftBorderBase + BottomCorner_TopMiddle,
       kBottomLeftBorderBase + BottomCorner_TopInner});

  // Bottom border
  Quad(kBottomLeftBorderBase + BottomCorner_BottomOuter,
       kBottomLeftBorderBase + BottomCorner_Center, kBottomRightBorderBase + BottomCorner_Center,
       kBottomRightBorderBase + BottomCorner_BottomOuter);

  Quad(kBottomLeftBorderBase + BottomCorner_Center, kBottomLeftBorderBase + BottomCorner_TopInner,
       kBottomRightBorderBase + BottomCorner_TopInner,
       kBottomRightBorderBase + BottomCorner_Center);

  // Bottom right corner
  // TODO: change order
  Fan(kBottomRightBorderBase + BottomCorner_Center,
      {kBottomRightBorderBase + BottomCorner_BottomOuter,
       kBottomRightBorderBase + BottomCorner_TopOuter,
       kBottomRightBorderBase + BottomCorner_TopMiddle,
       kBottomRightBorderBase + BottomCorner_TopInner});

  // Right border
  Quad(kTopRightBorderBase + TopBorder_Outer, kTopRightBorderBase + TopBorder_Middle,
       kBottomRightBorderBase + BottomCorner_TopMiddle,
       kBottomRightBorderBase + BottomCorner_TopOuter);
  Quad(kTopRightBorderBase + TopBorder_Middle, kTopRightBorderBase + TopBorder_Inner,
       kBottomRightBorderBase + BottomCorner_TopInner,
       kBottomRightBorderBase + BottomCorner_TopMiddle);

  // Fill
  Quad(kFillBase, kFillBase + 1, kFillBase + 2, kFillBase + 3);

  return builder.detach();
}

void TitleButton::DrawButtonFace(SkCanvas& canvas, SkColor bg, SkColor fg) const {
  union RRect oval = {.sk = RRect()};
  float press_shift_y = PressRatio() * -kPressOffset;
  auto pressed_oval = oval.sk.makeOffset(0, press_shift_y);
  float lightness_adjust = clickable.highlight * 5;
  Vec2 gradient_center = oval.rect.TopCenter() + Vec2(0, -0.5_mm);
  float gradient_radius = oval.rect.Height();

  {  // gradient
    SkPaint paint;
    SkColor colors[2] = {color::AdjustLightness(bg, lightness_adjust + 10),   // top
                         color::AdjustLightness(bg, lightness_adjust - 10)};  // bottom
    sk_sp<SkShader> gradient = SkGradientShader::MakeRadial(gradient_center, gradient_radius,
                                                            colors, nullptr, 2, SkTileMode::kClamp);
    paint.setShader(gradient);
    canvas.drawRRect(pressed_oval, paint);
  }

  {  // soft shadow around the edges
    SkPaint paint;
    paint.setMaskFilter(SkMaskFilter::MakeBlur(kOuter_SkBlurStyle, 0.5_mm));
    SkColor colors[2] = {color::AdjustLightness(bg, lightness_adjust + 40),   // top
                         color::AdjustLightness(bg, lightness_adjust - 30)};  // bottom
    sk_sp<SkShader> gradient = SkGradientShader::MakeRadial(gradient_center, gradient_radius,
                                                            colors, nullptr, 2, SkTileMode::kClamp);
    paint.setShader(gradient);

    SkPath path = SkPath::RRect(pressed_oval);
    path.toggleInverseFillType();
    canvas.drawPath(path, paint);
  }

  {  // white border
    SkPaint paint;
    paint.setColor(SK_ColorWHITE);
    paint.setStyle(SkPaint::kStroke_Style);
    paint.setAntiAlias(true);
    paint.setStrokeWidth(0);
    canvas.drawRRect(pressed_oval, paint);
  }
}
}  // namespace automat::theme::xp