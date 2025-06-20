// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_tesseract_ocr.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkColorType.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkShader.h>
#include <include/core/SkVertices.h>
#include <include/effects/SkTrimPathEffect.h>
#include <leptonica/allheaders.h>
#include <leptonica/pix.h>
#include <tesseract/ocrclass.h>

#include <cmath>

#include "action.hh"
#include "animation.hh"
#include "argument.hh"
#include "color.hh"
#include "connector_optical.hh"
#include "embedded.hh"
#include "font.hh"
#include "gui_constants.hh"
#include "image_provider.hh"
#include "log.hh"
#include "str.hh"
#include "textures.hh"
#include "time.hh"

using namespace std;

namespace automat::library {

struct ImageArgument : Argument {
  TextDrawable icon;
  ImageArgument()
      : Argument("image", kRequiresObject), icon("IMG", gui::kLetterSize, gui::GetFont()) {
    requirements.push_back([](Location* location, Object* object, std::string& error) {
      // if (!object->AsImageProvider()) {
      //   error = "Object must provide images";
      //   return false;
      // }
      return true;
    });
  }
  PaintDrawable& Icon() override { return icon; }
};

struct TextArgument : Argument {
  TextDrawable icon;
  TextArgument() : Argument("text", kRequiresObject), icon("T", gui::kLetterSize, gui::GetFont()) {
    requirements.push_back([](Location* location, Object* object, std::string& error) {
      return true;  // Any object can receive text
    });
  }
  PaintDrawable& Icon() override { return icon; }
};

static ImageArgument image_arg;
static TextArgument text_arg;

struct TesseractWidget;

TesseractOCR::TesseractOCR() {
  auto eng_traineddata = embedded::assets_eng_traineddata.content;
  if (tesseract.Init(eng_traineddata.data(), eng_traineddata.size(), "eng",
                     tesseract::OEM_LSTM_ONLY, nullptr, 0, nullptr, nullptr, true, nullptr)) {
    LOG << "Tesseract init failed";
  }
}

std::string_view TesseractOCR::Name() const { return "Tesseract OCR"; }

Ptr<Object> TesseractOCR::Clone() const {
  auto ret = MakePtr<TesseractOCR>();
  ret->x_min_ratio = x_min_ratio;
  ret->x_max_ratio = x_max_ratio;
  ret->y_min_ratio = y_min_ratio;
  ret->y_max_ratio = y_max_ratio;
  ret->ocr_text = ocr_text;
  return ret;
}

struct TesseractWidget : Object::FallbackWidget, gui::PointerMoveCallback {
  constexpr static float kSize = 5_cm;
  constexpr static float kRegionStrokeWidth = 1_mm;
  constexpr static float kHandleSize = 3_mm;
  constexpr static float kEdgeWidth = 1_mm;
  constexpr static float kOuterSidesWidth = 5_mm;

  sk_sp<SkImage> source_image;  // Local copy of the source image

  enum class DragMode { None, Top, Bottom, Left, Right, Move };

  mutable DragMode hover_mode = DragMode::None;
  DragMode drag_mode = DragMode::None;
  Vec2 drag_start_pos;
  float drag_start_x_min, drag_start_x_max, drag_start_y_min, drag_start_y_max;
  Rect region_rect;
  std::string ocr_text;
  Optional<gui::Pointer::IconOverride> icon_override;
  animation::SpringV2<float> aspect_ratio = 1.618f;
  Rect status_rect;
  Optional<float> status_progress_ratio;

  // Number from 0 to 1, used to highlight a section of wireframe while OCR is running
  float laser_phase = 0.0f;
  float laser_alpha = 0.0f;

  enum AxisX : U8 {
    Left = 0,
    Right = 1,
  };
  enum AxisY : U8 {
    Bottom = 0,
    Top = 1,
  };
  enum AxisZ : U8 {
    Back = 0,
    Front = 1,
  };
  enum AxisW : U8 {
    Outer = 0,
    Inner = 1,
  };
  enum AxisBoth {
    Both = 0,
  };
  struct TesseractPoints {
    Vec2 vertices[16];
    Vec2& operator[](AxisX x, AxisY y, AxisZ z, AxisW w) {
      return vertices[x * 8 + y * 4 + z * 2 + w];
    }
    const Vec2& operator[](AxisX x, AxisY y, AxisZ z, AxisW w) const {
      return vertices[x * 8 + y * 4 + z * 2 + w];
    }
    template <typename WallSubtype>
    struct WallBase {
      const TesseractPoints& points;
      bool Contains(Vec2 x) const {
        auto& wall = static_cast<const WallSubtype&>(*this);
        U8 cross_signs = 0;
        for (U8 i = 0; i < 4; ++i) {
          auto& a = wall[i];
          auto& b = wall[(i + 1) & 3];
          cross_signs |= signbit(Cross(b - a, x - a)) << i;
        }
        return cross_signs == 0b1111 || cross_signs == 0b0000;
      }
    };
    // Describes a wall orthogonal to the Y axis
    struct WallY : WallBase<WallY> {
      AxisY y;
      AxisW w;
      const Vec2& operator[](U8 i) const {
        return points[(AxisX)(__builtin_parity(i)), y, (AxisZ)((i >> 1) & 1), w];
      }
    };
    struct WallX : WallBase<WallX> {
      AxisX x;
      AxisW w;
      const Vec2& operator[](U8 i) const {
        return points[x, (AxisY)(__builtin_parity(i)), (AxisZ)((i >> 1) & 1), w];
      }
    };
    struct WallZ : WallBase<WallZ> {
      AxisZ z;
      AxisW w;
      const Vec2& operator[](U8 i) const {
        return points[(AxisX)(__builtin_parity(i)), (AxisY)((i >> 1) & 1), z, w];
      }
    };
    WallX Wall(AxisX x, AxisW w) const { return WallX{*this, x, w}; }
    WallY Wall(AxisY y, AxisW w) const { return WallY{*this, y, w}; }
    WallZ Wall(AxisZ z, AxisW w) const { return WallZ{*this, z, w}; }
  } points;

  Ptr<TesseractOCR> LockTesseract() const { return LockObject<TesseractOCR>(); }

  TesseractWidget(WeakPtr<Object> tesseract) { object = tesseract; }

  constexpr static RRect kBounds = RRect::MakeSimple(Rect::MakeAtZero({kSize, kSize}), 0);

  RRect CoarseBounds() const override { return kBounds; }
  Rect OuterRect() const {
    float width, height;
    if (aspect_ratio == 1) {
      width = height = kSize;
    } else if (aspect_ratio > 1) {
      width = kSize;
      height = width / aspect_ratio;
    } else {
      height = kSize;
      width = height * aspect_ratio;
    }
    return Rect::MakeAtZero({width, height});
  }
  SkPath Shape() const override { return SkPath::Rect(OuterRect()); }

  DragMode GetDragModeAt(Vec2 pos) const {
    if (points.Wall(Top, Inner).Contains(pos)) return DragMode::Top;
    if (points.Wall(Bottom, Inner).Contains(pos)) return DragMode::Bottom;
    if (points.Wall(Left, Inner).Contains(pos)) return DragMode::Left;
    if (points.Wall(Right, Inner).Contains(pos)) return DragMode::Right;
    if (points.Wall(Back, Inner).Contains(pos)) return DragMode::Move;
    return DragMode::None;
  }

  gui::Pointer::IconType GetCursorForMode(DragMode mode) const {
    switch (mode) {
      case DragMode::Top:
      case DragMode::Bottom:
        return gui::Pointer::kIconResizeVertical;
      case DragMode::Left:
      case DragMode::Right:
        return gui::Pointer::kIconResizeHorizontal;
      case DragMode::Move:
        return gui::Pointer::kIconAllScroll;
      default:
        return gui::Pointer::kIconArrow;
    }
  }

  animation::Phase Tick(time::Timer& timer) override {
    auto phase = animation::Finished;
    // Copy source image from connected image provider
    if (auto tesseract = LockTesseract()) {
      if (tesseract->status_mutex.try_lock()) {
        status_rect = tesseract->status_rect;
        status_progress_ratio = tesseract->status_progress_ratio;
        tesseract->status_mutex.unlock();
      }
      laser_phase = fmod(timer.NowSeconds(), 1.0f);
      if (status_progress_ratio.has_value()) {
        phase |= animation::Animating;
        laser_alpha = 1.0f;
      } else {
        phase |= animation::LinearApproach(0, timer.d, 2, laser_alpha);
      }
      region_rect.left = tesseract->x_min_ratio;
      region_rect.right = tesseract->x_max_ratio;
      region_rect.bottom = tesseract->y_min_ratio;
      region_rect.top = tesseract->y_max_ratio;
      ocr_text = tesseract->ocr_text;
      float target_aspect_ratio = 1.618f;
      if (auto here_ptr = tesseract->here.lock()) {
        auto image_obj = image_arg.FindObject(*here_ptr, {});
        if (image_obj) {
          auto image_provider = image_obj->AsImageProvider();
          if (image_provider) {
            source_image = image_provider->GetImage();
            if (source_image) {
              float image_width = source_image->width();
              float image_height = source_image->height();
              constexpr static float kMaxImageDimension = kSize - kEdgeWidth - kOuterSidesWidth * 2;
              if (image_width > image_height) {
                image_height = kMaxImageDimension * image_height / image_width;
                image_width = kMaxImageDimension;
              } else {
                image_width = kMaxImageDimension * image_width / image_height;
                image_height = kMaxImageDimension;
              }
              float width = image_width + kEdgeWidth + kOuterSidesWidth * 2;
              float height = image_height + kEdgeWidth + kOuterSidesWidth * 2;
              target_aspect_ratio = width / height;
            }
          }
        }
      }

      phase |= aspect_ratio.SpringTowards(target_aspect_ratio, timer.d, 0.3, 0.15);

      auto outer_front = OuterRect();
      outer_front = outer_front.Outset(-kEdgeWidth / 2);
      points[Left, Bottom, Front, Outer] = outer_front.BottomLeftCorner();
      points[Right, Bottom, Front, Outer] = outer_front.BottomRightCorner();
      points[Left, Top, Front, Outer] = outer_front.TopLeftCorner();
      points[Right, Top, Front, Outer] = outer_front.TopRightCorner();
      auto outer_back = outer_front.Outset(-kOuterSidesWidth);
      points[Left, Bottom, Back, Outer] = outer_back.BottomLeftCorner();
      points[Right, Bottom, Back, Outer] = outer_back.BottomRightCorner();
      points[Left, Top, Back, Outer] = outer_back.TopLeftCorner();
      points[Right, Top, Back, Outer] = outer_back.TopRightCorner();

      auto inner_back = Rect(lerp(outer_back.left, outer_back.right, region_rect.left),
                             lerp(outer_back.bottom, outer_back.top, region_rect.bottom),
                             lerp(outer_back.left, outer_back.right, region_rect.right),
                             lerp(outer_back.bottom, outer_back.top, region_rect.top));
      points[Left, Bottom, Back, Inner] = inner_back.BottomLeftCorner();
      points[Right, Bottom, Back, Inner] = inner_back.BottomRightCorner();
      points[Left, Top, Back, Inner] = inner_back.TopLeftCorner();
      points[Right, Top, Back, Inner] = inner_back.TopRightCorner();
      auto inner_front = inner_back.Outset(3_mm);
      points[Left, Bottom, Front, Inner] = inner_front.BottomLeftCorner();
      points[Right, Bottom, Front, Inner] = inner_front.BottomRightCorner();
      points[Left, Top, Front, Inner] = inner_front.TopLeftCorner();
      points[Right, Top, Front, Inner] = inner_front.TopRightCorner();
    }
    return phase;
  }

  void Draw(SkCanvas& canvas) const override {
    Vec2 image_size;
    sk_sp<SkShader> image_shader;
    // Draw background image if available
    if (source_image) {
      image_size.x = source_image->width();
      image_size.y = source_image->height();
      image_shader = source_image->makeShader(kFastSamplingOptions, nullptr);
    } else {
      image_size.x = 1;
      image_size.y = 1;
      image_shader = SkShaders::Color("#808080"_color);
    }

    {  // blurry background
      auto builder = SkVertices::Builder(
          SkVertices::kTriangles_VertexMode, 12, 30,
          SkVertices::kHasTexCoords_BuilderFlag | SkVertices::kHasColors_BuilderFlag);
      auto pos = builder.positions();
      pos[0] = points[Left, Top, Back, Outer];
      pos[1] = points[Right, Top, Back, Outer];
      pos[2] = points[Left, Bottom, Back, Outer];
      pos[3] = points[Right, Bottom, Back, Outer];

      pos[4] = points[Left, Top, Front, Outer];
      pos[5] = points[Right, Top, Front, Outer];
      pos[6] = points[Left, Bottom, Front, Outer];
      pos[7] = points[Right, Bottom, Front, Outer];

      pos[8] = pos[4];
      pos[9] = pos[5];
      pos[10] = pos[6];
      pos[11] = pos[7];

      constexpr static SkColor kBaseWallColor = "#00a3ff"_color;
      auto brighter = color::AdjustLightness(kBaseWallColor, 10);
      auto darker = color::AdjustLightness(kBaseWallColor, -20);
      auto darkest = color::AdjustLightness(kBaseWallColor, -50);
      auto darker_transparent = SkColorSetA(darker, 0x80);

      auto colors = builder.colors();
      colors[0] = darker_transparent;
      colors[1] = darker_transparent;
      colors[2] = darker_transparent;
      colors[3] = darker_transparent;

      colors[4] = darkest;
      colors[5] = darkest;
      colors[6] = brighter;
      colors[7] = brighter;

      colors[8] = darker;
      colors[9] = darker;
      colors[10] = kBaseWallColor;
      colors[11] = kBaseWallColor;

      auto tex_coords = builder.texCoords();
      tex_coords[0] = SkPoint::Make(0, 0);
      tex_coords[1] = SkPoint::Make(image_size.x, 0);
      tex_coords[2] = SkPoint::Make(0, image_size.y);
      tex_coords[3] = SkPoint::Make(image_size.x, image_size.y);

      tex_coords[4] = tex_coords[2];
      tex_coords[5] = tex_coords[3];
      tex_coords[6] = tex_coords[0];
      tex_coords[7] = tex_coords[1];

      tex_coords[8] = tex_coords[1];
      tex_coords[9] = tex_coords[0];
      tex_coords[10] = tex_coords[3];
      tex_coords[11] = tex_coords[2];

      auto ind = builder.indices();
      auto Face = [&](int start_index, int a, int b, int c, int d) {
        ind[start_index] = a;
        ind[start_index + 1] = b;
        ind[start_index + 2] = c;
        ind[start_index + 3] = b;
        ind[start_index + 4] = c;
        ind[start_index + 5] = d;
      };
      Face(0, 0, 1, 2, 3);    // back face
      Face(6, 0, 1, 4, 5);    // top face
      Face(12, 0, 2, 8, 10);  // left face
      Face(18, 1, 3, 9, 11);  // right face
      Face(24, 2, 3, 6, 7);   // bottom face

      SkPaint bg_paint;
      bg_paint.setImageFilter(SkImageFilters::Blur(0.25_mm, 0.25_mm, nullptr));
      bg_paint.setShader(image_shader);
      auto vertices = builder.detach();
      canvas.drawVertices(vertices.get(), SkBlendMode::kDstOver, bg_paint);
    }

    auto RectPath = [&](Vec2 p1, Vec2 p2, Vec2 p3, Vec2 p4) {
      SkPath border_path;
      border_path.moveTo((p1 + p2) / 2);
      border_path.arcTo(p2, (p2 + p3) / 2, kEdgeWidth / 2);
      border_path.arcTo(p3, (p4 + p3) / 2, kEdgeWidth / 2);
      border_path.arcTo(p4, (p1 + p4) / 2, kEdgeWidth / 2);
      border_path.arcTo(p1, (p1 + p2) / 2, kEdgeWidth / 2);
      border_path.close();
      return border_path;
    };
    static constexpr SkColor kInnerColor = "#ee7857"_color;
    auto color_outer = color::MakeTintFilter("#333333"_color, 80);
    auto color_outer_back = color::MakeTintFilter("#111111"_color, 20);
    auto color_inner = color::MakeTintFilter(kInnerColor, 20);
    auto color_inner_back = color::MakeTintFilter(kInnerColor, 10);
    auto color_inner_outer = color::MakeTintFilter("#111111"_color, 20);

    {  // sharp center
      auto builder = SkVertices::Builder(SkVertices::kTriangleStrip_VertexMode, 4, 0,
                                         SkVertices::kHasTexCoords_BuilderFlag);
      auto pos = builder.positions();
      pos[0] = points[Left, Top, Back, Inner];
      pos[1] = points[Right, Top, Back, Inner];
      pos[2] = points[Left, Bottom, Back, Inner];
      pos[3] = points[Right, Bottom, Back, Inner];

      auto tex_coords = builder.texCoords();
      tex_coords[0] =
          SkPoint::Make(region_rect.left * image_size.x, (1 - region_rect.top) * image_size.y);
      tex_coords[1] =
          SkPoint::Make(region_rect.right * image_size.x, (1 - region_rect.top) * image_size.y);
      tex_coords[2] =
          SkPoint::Make(region_rect.left * image_size.x, (1 - region_rect.bottom) * image_size.y);
      tex_coords[3] =
          SkPoint::Make(region_rect.right * image_size.x, (1 - region_rect.bottom) * image_size.y);

      SkPaint bg_paint;
      bg_paint.setShader(image_shader);
      auto vertices = builder.detach();
      canvas.drawVertices(vertices.get(), SkBlendMode::kSrc, bg_paint);

      Rect focus_rect;
      float w = image_size.x * region_rect.Width();
      float h = image_size.y * region_rect.Height();
      focus_rect.left = lerp(pos[0].x(), pos[1].x(), status_rect.left / w);
      focus_rect.right = lerp(pos[0].x(), pos[1].x(), status_rect.right / w);
      focus_rect.top = lerp(pos[2].y(), pos[0].y(), status_rect.top / h);
      focus_rect.bottom = lerp(pos[2].y(), pos[0].y(), status_rect.bottom / h);
      SkPaint paint;
      paint.setColor(kInnerColor);
      paint.setAlphaf(0.5);
      canvas.drawRect(focus_rect, paint);
    }

    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 2; ++j) {
        SkPath inner_outer;
        inner_outer.moveTo(points[(AxisX)i, (AxisY)j, Back, Inner]);
        inner_outer.lineTo(points[(AxisX)i, (AxisY)j, Back, Outer]);
        gui::DrawCable(canvas, inner_outer, color_inner_outer, CableTexture::Smooth,
                       kEdgeWidth * 0.5, kEdgeWidth * 0.5, nullptr);
      }
    }
    {    // sides of the inner cube
      {  // top
        SkPath path;
        path.moveTo(points[Left, Top, Back, Inner]);
        path.lineTo(points[Right, Top, Back, Inner]);
        path.lineTo(points[Right, Top, Front, Inner]);
        path.lineTo(points[Left, Top, Front, Inner]);
        path.close();
        SkPaint paint;
        paint.setColor("#6c2f1b"_color);
        paint.setAlphaf(0.5);
        canvas.drawPath(path, paint);
      }
      for (int i = 0; i < 2; ++i) {  // left & right
        SkPath path;
        path.moveTo(points[(AxisX)i, Top, Back, Inner]);
        path.lineTo(points[(AxisX)i, Top, Front, Inner]);
        path.lineTo(points[(AxisX)i, Bottom, Front, Inner]);
        path.lineTo(points[(AxisX)i, Bottom, Back, Inner]);
        path.close();
        SkPaint paint;
        paint.setColor("#a54b2f"_color);
        paint.setAlphaf(0.5);
        canvas.drawPath(path, paint);
      }
      {  // bottom
        SkPath path;
        path.moveTo(points[Left, Bottom, Back, Inner]);
        path.lineTo(points[Right, Bottom, Back, Inner]);
        path.lineTo(points[Right, Bottom, Front, Inner]);
        path.lineTo(points[Left, Bottom, Front, Inner]);
        path.close();
        SkPaint paint;
        paint.setColor("#ee7857"_color);
        paint.setAlphaf(0.5);
        canvas.drawPath(path, paint);
      }
    }

    auto outer_back =
        RectPath(points[Left, Top, Back, Outer], points[Right, Top, Back, Outer],
                 points[Right, Bottom, Back, Outer], points[Left, Bottom, Back, Outer]);
    gui::DrawCable(canvas, outer_back, color_outer_back, CableTexture::Smooth, kEdgeWidth * 0.5,
                   kEdgeWidth * 0.5, nullptr);

    auto inner_back =
        RectPath(points[Left, Top, Back, Inner], points[Right, Top, Back, Inner],
                 points[Right, Bottom, Back, Inner], points[Left, Bottom, Back, Inner]);
    gui::DrawCable(canvas, inner_back, color_inner_back, CableTexture::Smooth, kEdgeWidth * 0.5,
                   kEdgeWidth * 0.5, nullptr);

    for (int k = 0; k < 2; ++k) {
      for (int i = 0; i < 2; ++i) {
        for (int j = 0; j < 2; ++j) {
          SkPath front_back;
          front_back.moveTo(points[(AxisX)i, (AxisY)j, Back, (AxisW)k]);
          front_back.lineTo(points[(AxisX)i, (AxisY)j, Front, (AxisW)k]);
          gui::DrawCable(canvas, front_back, k == Inner ? color_inner : color_outer,
                         CableTexture::Smooth, kEdgeWidth * 0.5, kEdgeWidth, nullptr);
        }
      }
    }
    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 2; ++j) {
        SkPath inner_outer;
        inner_outer.moveTo(points[(AxisX)i, (AxisY)j, Front, Inner]);
        inner_outer.lineTo(points[(AxisX)i, (AxisY)j, Front, Outer]);
        gui::DrawCable(canvas, inner_outer, color_inner_outer, CableTexture::Smooth,
                       kEdgeWidth * 0.75, kEdgeWidth, nullptr);
      }
    }

    auto inner_front =
        RectPath(points[Left, Top, Front, Inner], points[Right, Top, Front, Inner],
                 points[Right, Bottom, Front, Inner], points[Left, Bottom, Front, Inner]);
    gui::DrawCable(canvas, inner_front, color_inner, CableTexture::Smooth, kEdgeWidth * 0.75,
                   kEdgeWidth * 0.75, nullptr);

    auto outer_front =
        RectPath(points[Left, Top, Front, Outer], points[Right, Top, Front, Outer],
                 points[Right, Bottom, Front, Outer], points[Left, Bottom, Front, Outer]);
    gui::DrawCable(canvas, outer_front, color_outer, CableTexture::Smooth, kEdgeWidth, kEdgeWidth,
                   nullptr);

    if (laser_alpha > 0.0f) {
      SkPath path;
      path.moveTo(points[Left, Top, Front, Outer]);
      path.lineTo(points[Right, Top, Front, Outer]);
      path.lineTo(points[Right, Top, Front, Inner]);
      path.lineTo(points[Right, Bottom, Front, Inner]);
      path.lineTo(points[Right, Bottom, Back, Inner]);
      path.lineTo(points[Left, Bottom, Back, Inner]);
      path.lineTo(points[Left, Bottom, Back, Outer]);
      path.lineTo(points[Left, Top, Back, Outer]);
      path.lineTo(points[Left, Top, Front, Outer]);
      path.lineTo(points[Left, Top, Front, Inner]);
      path.lineTo(points[Left, Top, Back, Inner]);
      path.lineTo(points[Right, Top, Back, Inner]);
      path.lineTo(points[Right, Top, Back, Outer]);
      path.lineTo(points[Right, Bottom, Back, Outer]);
      path.lineTo(points[Right, Bottom, Front, Outer]);
      path.lineTo(points[Left, Bottom, Front, Outer]);
      path.lineTo(points[Left, Bottom, Front, Inner]);
      path.lineTo(points[Left, Bottom, Back, Inner]);
      path.lineTo(points[Left, Top, Back, Inner]);
      path.lineTo(points[Left, Top, Back, Outer]);
      path.lineTo(points[Right, Top, Back, Outer]);
      path.lineTo(points[Right, Top, Front, Outer]);
      path.lineTo(points[Right, Bottom, Front, Outer]);
      path.lineTo(points[Right, Bottom, Front, Inner]);
      path.lineTo(points[Left, Bottom, Front, Inner]);
      path.lineTo(points[Left, Top, Front, Inner]);
      path.lineTo(points[Right, Top, Front, Inner]);
      path.lineTo(points[Right, Top, Back, Inner]);
      path.lineTo(points[Right, Bottom, Back, Inner]);
      path.lineTo(points[Right, Bottom, Back, Outer]);
      path.lineTo(points[Left, Bottom, Back, Outer]);
      path.lineTo(points[Left, Bottom, Front, Outer]);
      path.lineTo(points[Left, Top, Front, Outer]);
      path.close();
      float laser_width = status_progress_ratio.value_or(4.0f) / 4.f;
      float laser_start = laser_phase;
      float laser_end = laser_start + laser_width;
      auto mode = SkTrimPathEffect::Mode::kNormal;
      if (laser_end > 1.0f) {
        float tmp = laser_start;
        laser_start = laser_end - 1.0f;
        laser_end = tmp;
        mode = SkTrimPathEffect::Mode::kInverted;
      }
      auto effect = SkTrimPathEffect::Make(laser_start, laser_end, mode);
      SkPaint paint;
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(kEdgeWidth * 0.5);
      paint.setStrokeJoin(SkPaint::kRound_Join);
      paint.setAntiAlias(true);
      paint.setPathEffect(effect);

      // glow
      paint.setColor(kInnerColor);
      paint.setAlphaf(laser_alpha);
      paint.setMaskFilter(
          SkMaskFilter::MakeBlur(SkBlurStyle::kOuter_SkBlurStyle, kEdgeWidth * 0.5f));
      canvas.drawPath(path, paint);

      // white core
      paint.setColor("#ffffff"_color);
      paint.setAlphaf(laser_alpha);
      paint.setMaskFilter(nullptr);
      canvas.drawPath(path, paint);
    }

    return;

    SkPaint edge_paint;
    edge_paint.setStyle(SkPaint::kStroke_Style);
    edge_paint.setStrokeWidth(kEdgeWidth);
    edge_paint.setAntiAlias(true);

    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 2; ++j) {
        for (int k = 0; k < 2; ++k) {
          canvas.drawLine(points[(AxisX)i, (AxisY)j, (AxisZ)k, Inner],
                          points[(AxisX)i, (AxisY)j, (AxisZ)k, Outer], edge_paint);
          canvas.drawLine(points[(AxisX)i, (AxisY)j, Back, (AxisW)k],
                          points[(AxisX)i, (AxisY)j, Front, (AxisW)k], edge_paint);
          canvas.drawLine(points[(AxisX)i, Top, (AxisZ)j, (AxisW)k],
                          points[(AxisX)i, Bottom, (AxisZ)j, (AxisW)k], edge_paint);
          canvas.drawLine(points[Left, (AxisY)i, (AxisZ)j, (AxisW)k],
                          points[Right, (AxisY)i, (AxisZ)j, (AxisW)k], edge_paint);
        }
      }
    }

    // Draw OCR text if available
    if (!ocr_text.empty()) {
      auto& font = gui::GetFont();
      SkPaint text_paint;
      text_paint.setColor("#333333"_color);

      canvas.save();
      auto text_pos = kBounds.rect.BottomLeftCorner();
      text_pos.y += font.letter_height + 2_mm;
      canvas.translate(text_pos.x, text_pos.y);

      // Scale text to fit
      float text_width = font.MeasureText(ocr_text);
      float scale_x = std::min(1.0f, kSize / text_width);
      if (scale_x < 1.0f) {
        canvas.scale(scale_x, 1.0f);
      }

      font.DrawText(canvas, ocr_text, text_paint);
      canvas.restore();
    }

    DrawChildren(canvas);
  }

  void PointerOver(gui::Pointer& pointer) override {
    Vec2 pos = pointer.PositionWithin(*this);
    hover_mode = GetDragModeAt(pos);
    icon_override.emplace(pointer, GetCursorForMode(hover_mode));
    StartWatching(pointer);  // Start watching pointer movement
  }

  void PointerLeave(gui::Pointer& pointer) override {
    hover_mode = DragMode::None;
    icon_override.reset();  // Release the icon override
    StopWatching(pointer);  // Stop watching pointer movement
  }

  // PointerMoveCallback implementation
  void PointerMove(gui::Pointer& pointer, Vec2 position) override {
    Vec2 pos = pointer.PositionWithin(*this);
    DragMode new_mode = GetDragModeAt(pos);

    if (new_mode != hover_mode) {
      hover_mode = new_mode;
      // Update icon override with new cursor
      icon_override.emplace(pointer, GetCursorForMode(hover_mode));
    }
  }

  // Forward declaration for drag action
  struct RegionDragAction : Action {
    TesseractWidget& widget;
    DragMode mode;
    Vec2 last_pos;

    RegionDragAction(gui::Pointer& pointer, TesseractWidget& widget, DragMode mode)
        : Action(pointer), widget(widget), mode(mode) {
      last_pos = pointer.pointer_position;
    }
    void Update() override {
      auto min_corner = widget.points[Left, Bottom, Back, Outer];
      auto max_corner = widget.points[Right, Top, Back, Outer];
      Vec2 size = max_corner - min_corner;
      auto transform = TransformDown(widget);
      Vec2 old_pos = transform.mapPoint(last_pos);
      Vec2 new_pos = transform.mapPoint(pointer.pointer_position);
      last_pos = pointer.pointer_position;
      Vec2 delta = (new_pos - old_pos) / size;
      if (auto tesseract = widget.LockTesseract()) {
        {
          auto lock = std::lock_guard(tesseract->mutex);
          switch (mode) {
            case DragMode::Top:
              tesseract->y_max_ratio =
                  std::clamp(tesseract->y_max_ratio + delta.y, tesseract->y_min_ratio, 1.0f);
              break;
            case DragMode::Bottom:
              tesseract->y_min_ratio =
                  std::clamp(tesseract->y_min_ratio + delta.y, 0.0f, tesseract->y_max_ratio);
              break;
            case DragMode::Left:
              tesseract->x_min_ratio =
                  std::clamp(tesseract->x_min_ratio + delta.x, 0.0f, tesseract->x_max_ratio);
              break;
            case DragMode::Right:
              tesseract->x_max_ratio =
                  std::clamp(tesseract->x_max_ratio + delta.x, tesseract->x_min_ratio, 1.0f);
              break;
            case DragMode::Move:
              tesseract->y_max_ratio += delta.y;
              tesseract->y_min_ratio += delta.y;
              tesseract->x_min_ratio += delta.x;
              tesseract->x_max_ratio += delta.x;
              if (tesseract->y_max_ratio > 1.0f) {
                tesseract->y_min_ratio += 1.0f - tesseract->y_max_ratio;
                tesseract->y_max_ratio = 1.0f;
              }
              if (tesseract->y_min_ratio < 0.0f) {
                tesseract->y_max_ratio -= tesseract->y_min_ratio;
                tesseract->y_min_ratio = 0.0f;
              }
              if (tesseract->x_max_ratio > 1.0f) {
                tesseract->x_min_ratio += 1.0f - tesseract->x_max_ratio;
                tesseract->x_max_ratio = 1.0f;
              }
              if (tesseract->x_min_ratio < 0.0f) {
                tesseract->x_max_ratio -= tesseract->x_min_ratio;
                tesseract->x_min_ratio = 0.0f;
              }
              break;
            default:
              return;
          }
        }
        // tesseract->WakeWidgetsAnimation();
        tesseract->ForEachWidget([](gui::RootWidget&, gui::Widget& w) {
          w.WakeAnimation();
          w.RedrawThisFrame();
        });
      }
    }
  };

  std::unique_ptr<Action> FindAction(gui::Pointer& pointer, gui::ActionTrigger trigger) override {
    if (trigger == gui::PointerButton::Left) {
      Vec2 pos = pointer.PositionWithin(*this);
      DragMode mode = GetDragModeAt(pos);
      if (mode != DragMode::None) {
        return std::make_unique<RegionDragAction>(pointer, *this, mode);
      }
    }
    return FallbackWidget::FindAction(pointer, trigger);
  }

  Vec2AndDir ArgStart(const Argument& arg) override {
    if (&arg == &image_arg) {
      return Vec2AndDir{kBounds.rect.RightCenter(), 0_deg};
    }
    if (&arg == &text_arg) {
      return Vec2AndDir{kBounds.rect.LeftCenter(), 180_deg};
    }
    return FallbackWidget::ArgStart(arg);
  }
};

Ptr<gui::Widget> TesseractOCR::MakeWidget() {
  return MakePtr<TesseractWidget>(AcquireWeakPtr<Object>());
}

void TesseractOCR::Args(std::function<void(Argument&)> cb) {
  cb(image_arg);
  cb(text_arg);
  cb(next_arg);
}

void TesseractOCR::OnRun(Location& here) {
  auto image_obj = image_arg.FindObject(here, {});
  auto text_obj = text_arg.FindObject(here, {});

  if (!image_obj) {
    here.ReportError("No image source connected");
    return;
  }

  auto image_provider = image_obj->AsImageProvider();
  if (!image_provider) {
    here.ReportError("Connected object doesn't provide images");
    return;
  }

  auto image = image_provider->GetImage();
  if (!image) {
    here.ReportError("No image available from source");
    return;
  }

  int width = image->width();
  int height = image->height();
  if (width == 0 || height == 0) return;

  std::string utf8_text = "";
  auto pix = pixCreate(width, height, 32);
  uint32_t* pix_data = pixGetData(pix);
  SkPixmap pixmap;
  if (!image->peekPixels(&pixmap)) {
    pixDestroy(&pix);
    return;
  }

  auto pixInfo =
      SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType);
  pixmap.readPixels(pixInfo, pix_data, width * 4);

  int ocr_left = x_min_ratio * width;
  int ocr_top = (1 - y_max_ratio) * height;
  int ocr_width = width * (x_max_ratio - x_min_ratio);
  int ocr_height = height * (y_max_ratio - y_min_ratio);

  if (ocr_width > 0 && ocr_height > 0) {
    tesseract.SetImage(pix);
    tesseract.SetRectangle(ocr_left, ocr_top, ocr_width, ocr_height);

    tesseract::ETEXT_DESC monitor;
    monitor.cancel_this = this;
    monitor.progress_callback2 = [](tesseract::ETEXT_DESC* etext, int left, int right, int top,
                                    int bottom) {
      auto self = (TesseractOCR*)etext->cancel_this;
      if (self->status_mutex.try_lock()) {
        self->status_rect = Rect(left, bottom, right, top);
        self->status_progress_ratio = etext->progress / 100.0f;
        self->status_mutex.unlock();
        self->WakeWidgetsAnimation();
      }
      return false;
    };
    {
      auto lock = std::lock_guard(status_mutex);
      status_rect = Rect(0, 0, 0, 0);
      status_progress_ratio = 0.0f;
    }
    int recognize_status = tesseract.Recognize(&monitor);
    if (recognize_status) {
      LOG << "Tesseract recognize failed: " << recognize_status;
    }
    {
      auto lock = std::lock_guard(status_mutex);
      status_rect = Rect(0, 0, 0, 0);
      status_progress_ratio.reset();
    }
    utf8_text = tesseract.GetUTF8Text();
    StripTrailingWhitespace(utf8_text);
  }

  pixDestroy(&pix);

  {
    auto lock = std::lock_guard(mutex);
    ocr_text = utf8_text;
  }

  if (text_obj) {
    text_obj->SetText(here, utf8_text);
  }

  WakeWidgetsAnimation();
}

void TesseractOCR::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();

  writer.Key("ocr_text");
  writer.String(ocr_text.data(), ocr_text.size());
  writer.Key("x_min_ratio");
  writer.Double(x_min_ratio);
  writer.Key("x_max_ratio");
  writer.Double(x_max_ratio);
  writer.Key("y_min_ratio");
  writer.Double(y_min_ratio);
  writer.Key("y_max_ratio");
  writer.Double(y_max_ratio);

  writer.EndObject();
}

void TesseractOCR::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  for (auto key : ObjectView(d, status)) {
    if (key == "ocr_text") {
      d.Get(ocr_text, status);
    } else if (key == "x_min_ratio") {
      d.Get(x_min_ratio, status);
    } else if (key == "x_max_ratio") {
      d.Get(x_max_ratio, status);
    } else if (key == "y_min_ratio") {
      d.Get(y_min_ratio, status);
    } else if (key == "y_max_ratio") {
      d.Get(y_max_ratio, status);
    }
  }
  if (!OK(status)) {
    l.ReportError(status.ToStr());
  }
}

std::string TesseractOCR::GetText() const {
  auto lock = std::lock_guard(mutex);
  return ocr_text;
}

void TesseractOCR::SetText(Location& error_context, std::string_view text) {
  auto lock = std::lock_guard(mutex);
  ocr_text = text;
  WakeWidgetsAnimation();
}

}  // namespace automat::library