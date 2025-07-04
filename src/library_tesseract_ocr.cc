// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_tesseract_ocr.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkColorType.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkShader.h>
#include <include/core/SkVertices.h>
#include <include/effects/SkTrimPathEffect.h>
#include <include/pathops/SkPathOps.h>
#include <leptonica/allheaders.h>
#include <leptonica/pix.h>
#include <tesseract/ocrclass.h>
#include <tesseract/resultiterator.h>

#include <cmath>
#include <tracy/Tracy.hpp>

#include "action.hh"
#include "animation.hh"
#include "argument.hh"
#include "automat.hh"
#include "color.hh"
#include "connector_optical.hh"
#include "embedded.hh"
#include "font.hh"
#include "gui_constants.hh"
#include "image_provider.hh"
#include "log.hh"
#include "str.hh"
#include "svg.hh"
#include "textures.hh"
#include "time.hh"

using namespace std;

namespace automat::library {

constexpr bool kDebugEyeShape = false;

struct ImageArgument : LiveArgument {
  TextDrawable icon;
  ImageArgument()
      : LiveArgument("image", kRequiresObject), icon("IMG", gui::kLetterSize, gui::GetFont()) {
    requirements.push_back([](Location* location, Object* object, std::string& error) {
      if (!object->AsImageProvider()) {
        error = "Object must provide images";
        return false;
      }
      return true;
    });
    autoconnect_radius = 20_cm;
    style = Style::Invisible;
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
  constexpr static SkColor kInnerColor = "#ee7857"_color;
  constexpr static SkColor kBaseWallColor = "#356570"_color;

  sk_sp<SkImage> source_image;  // Local copy of the source image

  enum class DragMode { None, Top, Bottom, Left, Right, Move };

  mutable DragMode hover_mode = DragMode::None;
  DragMode drag_mode = DragMode::None;
  Vec2 drag_start_pos;
  float drag_start_x_min, drag_start_x_max, drag_start_y_min, drag_start_y_max;
  Rect region_rect;
  Optional<Vec2> iris_target;  // Where the eye is pointing (machine coords)
  animation::SpringV2<Vec2> iris_dir;
  std::string ocr_text;
  Optional<gui::Pointer::IconOverride> icon_override;
  animation::SpringV2<float> aspect_ratio = 1.618f;
  Rect status_rect;
  Optional<float> status_progress_ratio;
  Vec<TesseractOCR::RecognitionResult> status_results;

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
  };

  Ptr<TesseractOCR> LockTesseract() const { return LockObject<TesseractOCR>(); }

  TesseractWidget(WeakPtr<Object> tesseract) { object = tesseract; }

  constexpr static RRect kBounds = RRect::MakeSimple(Rect::MakeAtZero({kSize, kSize}), 0);

  RRect CoarseBounds() const override {
    auto r = layout.border_outer;
    r.top += EyeImage().height() / 2;
    return RRect::MakeSimple(r, 0);
  }

  static PersistentImage& BorderImage() {
    static auto image =
        PersistentImage::MakeFromAsset(embedded::assets_ocr_border_webp, {.width = kSize});
    return image;
  }

  static PersistentImage& EyeImage() {
    static auto image =
        PersistentImage::MakeFromAsset(embedded::assets_ocr_eye_webp, {.width = kSize / 5});
    return image;
  }

  static PersistentImage& IrisImage() {
    static auto image =
        PersistentImage::MakeFromAsset(embedded::assets_ocr_iris_webp, {.width = kSize / 14});
    return image;
  }

  static const SkPath kEyeShape;

  static PersistentImage& BoxImage() {
    static auto image =
        PersistentImage::MakeFromAsset(embedded::assets_ocr_box_webp, {.width = 1287});
    return image;
  }

  Optional<Rect> TextureBounds() const override {
    auto r = layout.border_outer;
    r.top += EyeImage().height() / 2;
    return r;
  }
  struct Layout {
    float aspect_ratio;
    Rect region_rect;

    // Computed values:
    mutable Rect border_outer;  // a rectangle that fully contains the tesseract's outer border
    mutable Rect border_inner;
    mutable SkPath shape;
    mutable Vec2 eye_center;
    mutable TesseractPoints points;

    Layout() : Layout(1.618f, Rect(0.25, 0.25, 0.75, 0.75)) {}
    Layout(float aspect_ratio, Rect region_rect)
        : aspect_ratio(aspect_ratio), region_rect(region_rect) {
      {  // border_outer, border_inner
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
        border_outer = Rect::MakeAtZero({width, height});
        border_inner = border_outer.Inset(kEdgeWidth);
      }
      {  // points, eye_center
        Rect outer_front = border_outer.Inset(kEdgeWidth / 2);
        Rect outer_back = outer_front.Outset(-kOuterSidesWidth);
        eye_center = outer_front.TopCenter();

        points[Left, Bottom, Front, Outer] = outer_front.BottomLeftCorner();
        points[Right, Bottom, Front, Outer] = outer_front.BottomRightCorner();
        points[Left, Top, Front, Outer] = outer_front.TopLeftCorner();
        points[Right, Top, Front, Outer] = outer_front.TopRightCorner();
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
      auto rect_shape = SkPath::Rect(border_outer);
      auto eye_shape = kEyeShape.makeTransform(SkMatrix::Translate(eye_center).preScale(1.5, 1.5));
      Op(rect_shape, eye_shape, kUnion_SkPathOp, &shape);
    }

    TesseractPoints::WallX Wall(AxisX x, AxisW w) const {
      return TesseractPoints::WallX{points, x, w};
    }
    TesseractPoints::WallY Wall(AxisY y, AxisW w) const {
      return TesseractPoints::WallY{points, y, w};
    }
    TesseractPoints::WallZ Wall(AxisZ z, AxisW w) const {
      return TesseractPoints::WallZ{points, z, w};
    }
    const Vec2& operator[](AxisX x, AxisY y, AxisZ z, AxisW w) const { return points[x, y, z, w]; }
  };

  Layout layout;

  SkPath Shape() const override { return layout.shape; }

  DragMode GetDragModeAt(Vec2 pos) const {
    if (layout.Wall(Top, Inner).Contains(pos)) return DragMode::Top;
    if (layout.Wall(Bottom, Inner).Contains(pos)) return DragMode::Bottom;
    if (layout.Wall(Left, Inner).Contains(pos)) return DragMode::Left;
    if (layout.Wall(Right, Inner).Contains(pos)) return DragMode::Right;
    if (layout.Wall(Back, Inner).Contains(pos)) return DragMode::Move;
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

  void TransformUpdated() override { WakeAnimation(); }

  animation::Phase Tick(time::Timer& timer) override {
    auto phase = animation::Finished;
    // Copy source image from connected image provider
    if (auto tesseract = LockTesseract()) {
      if (tesseract->status_mutex.try_lock()) {
        status_rect = tesseract->status_rect;
        status_progress_ratio = tesseract->status_progress_ratio;
        status_results = tesseract->status_results;
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
      iris_target.reset();
      {  // Update `source_image`
        sk_sp<SkImage> new_image = nullptr;
        if (auto here_ptr = tesseract->here.lock()) {
          auto image_loc = image_arg.FindLocation(*here_ptr, {});
          auto image_obj = image_loc ? image_loc->object.get() : nullptr;
          if (image_obj) {
            auto image_provider = image_obj->AsImageProvider();
            if (image_provider) {
              new_image = image_provider->GetImage();
              iris_target = image_loc->position;
            }
          }

          if (status_progress_ratio.has_value()) {
            iris_target = here_ptr->position;
          }
        }
        source_image = std::move(new_image);
      }

      if (pointers.size() > 0) {
        auto pointer_pos = pointers.front()->PositionWithin(*root_machine);
        iris_target = pointer_pos;
      }

      if (source_image) {  // Update `aspect_ratio`
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
      } else {
        target_aspect_ratio = 1.618f;
      }

      phase |= aspect_ratio.SineTowards(target_aspect_ratio, timer.d, 0.3);

      layout = Layout(aspect_ratio, region_rect);

      {  // animate iris
        Vec2 eye_delta;
        if (iris_target.has_value()) {
          auto matrix = TransformBetween(*root_machine, *this);
          eye_delta = matrix.mapPoint(iris_target->sk) - layout.eye_center;
        } else {
          eye_delta = Vec2(0, 0);
        }

        auto eye_dir = Normalize(eye_delta);
        float z = 2_cm;
        auto eye_dist_3d = Length(Vec3(eye_delta.x, eye_delta.y, z));
        auto eye_dist_2d = Length(eye_delta);

        float dist = eye_dist_2d / eye_dist_3d;

        Vec2 iris_dir_target = Vec2(eye_dir.x * dist, eye_dir.y * dist);
        phase |= iris_dir.SineTowards(iris_dir_target, timer.d, 1);
      }
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
      image_shader = SkShaders::Color("#80808000"_color);
    }

    auto& box_image = BoxImage();
    auto box_back = Rect(214, 127, 1082, 654);
    auto DrawQuad = [&](Vec2 pts[4], Vec2 tex_pts[4]) {
      SkMatrix m;
      m.setPolyToPoly((SkPoint*)tex_pts, (SkPoint*)pts, 4);
      auto builder = SkVertices::Builder(SkVertices::kTriangleFan_VertexMode, 4, 0, 0);
      auto pos = builder.positions();
      for (int i = 0; i < 4; ++i) {
        pos[i] = tex_pts[i];
      }
      SkPaint paint;
      paint.setShader(*box_image.shader);
      canvas.save();
      canvas.concat(m);
      canvas.drawVertices(builder.detach(), SkBlendMode::kSrc, paint);
      canvas.restore();
    };

    {  // left wall
      auto left_wall = layout.Wall(Left, Outer);
      Vec2 tex_pts[4] = {
          Vec2(0, 0),
          box_back.BottomLeftCorner(),
          box_back.TopLeftCorner(),
          Vec2(0, box_image.height()),
      };
      Vec2 pts[4] = {
          left_wall[3],
          left_wall[0],
          left_wall[1],
          left_wall[2],
      };
      DrawQuad(pts, tex_pts);
    }
    {  // right wall
      auto right_wall = layout.Wall(Right, Outer);
      Vec2 tex_pts[4] = {
          Vec2(box_image.width(), 0),
          box_back.BottomRightCorner(),
          box_back.TopRightCorner(),
          Vec2(box_image.width(), box_image.height()),
      };
      Vec2 pts[4] = {
          right_wall[3],
          right_wall[0],
          right_wall[1],
          right_wall[2],
      };
      DrawQuad(pts, tex_pts);
    }
    {  // bottom wall
      auto bottom_wall = layout.Wall(Bottom, Outer);
      Vec2 tex_pts[4] = {
          Vec2(0, 0),
          box_back.BottomLeftCorner(),
          box_back.BottomRightCorner(),
          Vec2(box_image.width(), 0),
      };
      Vec2 pts[4] = {
          bottom_wall[3],
          bottom_wall[0],
          bottom_wall[1],
          bottom_wall[2],
      };
      DrawQuad(pts, tex_pts);
    }
    {  // top wall
      auto top_wall = layout.Wall(Top, Outer);
      Vec2 tex_pts[4] = {
          Vec2(0, box_image.height()),
          box_back.TopLeftCorner(),
          box_back.TopRightCorner(),
          Vec2(box_image.width(), box_image.height()),
      };
      Vec2 pts[4] = {
          top_wall[3],
          top_wall[0],
          top_wall[1],
          top_wall[2],
      };
      DrawQuad(pts, tex_pts);
    }
    {  // back wall
      auto back_wall = layout.Wall(Back, Outer);
      Vec2 tex_pts[4] = {
          box_back.TopLeftCorner(),
          box_back.BottomLeftCorner(),
          box_back.BottomRightCorner(),
          box_back.TopRightCorner(),
      };
      Vec2 pts[4] = {
          back_wall[3],
          back_wall[0],
          back_wall[1],
          back_wall[2],
      };
      DrawQuad(pts, tex_pts);
    }

    {  // blurry background
      auto builder = SkVertices::Builder(SkVertices::kTriangles_VertexMode, 4, 6,
                                         SkVertices::kHasTexCoords_BuilderFlag);
      auto pos = builder.positions();
      pos[0] = layout[Left, Top, Back, Outer];
      pos[1] = layout[Right, Top, Back, Outer];
      pos[2] = layout[Left, Bottom, Back, Outer];
      pos[3] = layout[Right, Bottom, Back, Outer];

      auto tex_coords = builder.texCoords();
      tex_coords[0] = SkPoint::Make(0, 0);
      tex_coords[1] = SkPoint::Make(image_size.x, 0);
      tex_coords[2] = SkPoint::Make(0, image_size.y);
      tex_coords[3] = SkPoint::Make(image_size.x, image_size.y);

      auto ind = builder.indices();
      auto Face = [&](int start_index, int a, int b, int c, int d) {
        ind[start_index] = a;
        ind[start_index + 1] = b;
        ind[start_index + 2] = c;
        ind[start_index + 3] = b;
        ind[start_index + 4] = c;
        ind[start_index + 5] = d;
      };
      Face(0, 0, 1, 2, 3);  // back face

      SkPaint bg_paint;
      bg_paint.setImageFilter(SkImageFilters::Blur(0.25_mm, 0.25_mm, nullptr));
      bg_paint.setColorFilter(color::MakeTintFilter(kBaseWallColor, 30));
      bg_paint.setShader(image_shader);
      auto vertices = builder.detach();
      canvas.drawVertices(vertices.get(), SkBlendMode::kDstOver, bg_paint);
    }

    auto RectPath = [&](Vec2 p1, Vec2 p2, Vec2 p3, Vec2 p4, float radius = kEdgeWidth / 2) {
      SkPath border_path;
      border_path.moveTo((p1 + p2) / 2);
      if (radius > 0) {
        border_path.arcTo(p2, (p2 + p3) / 2, radius);
        border_path.arcTo(p3, (p4 + p3) / 2, radius);
        border_path.arcTo(p4, (p1 + p4) / 2, radius);
        border_path.arcTo(p1, (p1 + p2) / 2, radius);
      } else {
        border_path.lineTo(p2);
        border_path.lineTo(p3);
        border_path.lineTo(p4);
        border_path.lineTo(p1);
      }
      border_path.close();
      return border_path;
    };
    auto color_outer = color::MakeTintFilter("#333333"_color, 80);
    auto color_outer_back = color::MakeTintFilter("#111111"_color, 20);
    auto color_inner = color::MakeTintFilter(kInnerColor, 40);
    auto color_inner_back = color::MakeTintFilter(kInnerColor, 20);
    auto color_inner_outer = color::MakeTintFilter("#444444"_color, 20);

    {  // sharp center
      auto builder = SkVertices::Builder(SkVertices::kTriangleStrip_VertexMode, 4, 0,
                                         SkVertices::kHasTexCoords_BuilderFlag);
      auto pos = builder.positions();
      pos[0] = layout[Left, Top, Back, Inner];
      pos[1] = layout[Right, Top, Back, Inner];
      pos[2] = layout[Left, Bottom, Back, Inner];
      pos[3] = layout[Right, Bottom, Back, Inner];

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

    if (status_results.size() > 0) {  // draw results
      auto& font = gui::GetFont();
      SkPaint paint_bg;
      paint_bg.setColor(color::FastMix("#00000080"_color, "#00000000"_color, laser_alpha));
      SkPaint paint;
      paint.setColor(color::FastMix(kInnerColor, SK_ColorWHITE, laser_alpha));
      // paint.setAlphaf(0.5f);
      auto matrix = canvas.getLocalToDevice();
      auto min_corner = layout[Left, Bottom, Back, Outer];
      auto max_corner = layout[Right, Top, Back, Outer];
      auto size = max_corner - min_corner;
      canvas.save();
      auto min_clip = layout[Left, Bottom, Back, Inner];
      auto max_clip = layout[Right, Top, Back, Inner];
      canvas.clipRect(Rect(min_clip.x, min_clip.y, max_clip.x, max_clip.y));
      for (auto& result : status_results) {
        float width = font.MeasureText(result.text);
        float left = min_corner.x + size.x * (result.rect.left / image_size.x);
        float right = min_corner.x + size.x * (result.rect.right / image_size.x);
        float bottom = min_corner.y + size.y * (1 - result.rect.bottom / image_size.y);
        float top = min_corner.y + size.y * (1 - result.rect.top / image_size.y);
        auto text_matrix = matrix;
        auto src_rect = Rect(0, -font.descent, width, font.letter_height);
        auto dst_rect = Rect(left, bottom, right, top);
        text_matrix.preConcat(
            SkMatrix::MakeRectToRect(src_rect, dst_rect, SkMatrix::kFill_ScaleToFit));
        canvas.setMatrix(matrix);
        canvas.drawRect(dst_rect, paint_bg);
        canvas.setMatrix(text_matrix);
        font.DrawText(canvas, result.text, paint);
      }
      canvas.restore();
    }

    canvas.save();
    canvas.clipRect(layout.border_inner);

    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 2; ++j) {
        SkPath inner_outer;
        inner_outer.moveTo(layout[(AxisX)i, (AxisY)j, Back, Inner]);
        inner_outer.lineTo(layout[(AxisX)i, (AxisY)j, Back, Outer]);
        gui::DrawCable(canvas, inner_outer, color_inner_outer, CableTexture::Braided,
                       kEdgeWidth * 0.5, kEdgeWidth * 0.5, nullptr);
      }
    }
    {    // sides of the inner cube
      {  // top
        SkPath path;
        path.moveTo(layout[Left, Top, Back, Inner]);
        path.lineTo(layout[Right, Top, Back, Inner]);
        path.lineTo(layout[Right, Top, Front, Inner]);
        path.lineTo(layout[Left, Top, Front, Inner]);
        path.close();
        SkPaint paint;
        paint.setColor("#6c2f1b"_color);
        paint.setAlphaf(0.5);
        canvas.drawPath(path, paint);
      }
      for (int i = 0; i < 2; ++i) {  // left & right
        SkPath path;
        path.moveTo(layout[(AxisX)i, Top, Back, Inner]);
        path.lineTo(layout[(AxisX)i, Top, Front, Inner]);
        path.lineTo(layout[(AxisX)i, Bottom, Front, Inner]);
        path.lineTo(layout[(AxisX)i, Bottom, Back, Inner]);
        path.close();
        SkPaint paint;
        paint.setColor("#a54b2f"_color);
        paint.setAlphaf(0.5);
        canvas.drawPath(path, paint);
      }
      {  // bottom
        SkPath path;
        path.moveTo(layout[Left, Bottom, Back, Inner]);
        path.lineTo(layout[Right, Bottom, Back, Inner]);
        path.lineTo(layout[Right, Bottom, Front, Inner]);
        path.lineTo(layout[Left, Bottom, Front, Inner]);
        path.close();
        SkPaint paint;
        paint.setColor("#ee7857"_color);
        paint.setAlphaf(0.5);
        canvas.drawPath(path, paint);
      }
    }

    auto inner_back =
        RectPath(layout[Left, Top, Back, Inner], layout[Right, Top, Back, Inner],
                 layout[Right, Bottom, Back, Inner], layout[Left, Bottom, Back, Inner], 0);
    auto inner_back_arcline = ArcLine::MakeFromPath(inner_back);
    inner_back_arcline.Outset(kEdgeWidth * 0.25);
    inner_back = inner_back_arcline.ToPath();
    gui::DrawCable(canvas, inner_back, color_inner_back, CableTexture::Smooth, kEdgeWidth * 0.5,
                   kEdgeWidth * 0.5, nullptr);

    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 2; ++j) {
        SkPath front_back;
        front_back.moveTo(layout[(AxisX)i, (AxisY)j, Back, Inner]);
        front_back.lineTo(layout[(AxisX)i, (AxisY)j, Front, Inner]);
        gui::DrawCable(canvas, front_back, color_inner, CableTexture::Smooth, kEdgeWidth * 0.5,
                       kEdgeWidth, nullptr);
      }
    }
    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 2; ++j) {
        SkPath inner_outer;
        inner_outer.moveTo(layout[(AxisX)i, (AxisY)j, Front, Inner]);
        inner_outer.lineTo(layout[(AxisX)i, (AxisY)j, Front, Outer]);
        gui::DrawCable(canvas, inner_outer, color_inner_outer, CableTexture::Braided,
                       kEdgeWidth * 0.75, kEdgeWidth, nullptr);
      }
    }
    canvas.restore();

    auto inner_front =
        RectPath(layout[Left, Top, Front, Inner], layout[Right, Top, Front, Inner],
                 layout[Right, Bottom, Front, Inner], layout[Left, Bottom, Front, Inner]);
    gui::DrawCable(canvas, inner_front, color_inner, CableTexture::Smooth, kEdgeWidth * 0.75,
                   kEdgeWidth * 0.75, nullptr);

    auto& border_image = BorderImage();
    canvas.save();
    canvas.concat(SkMatrix::MakeRectToRect(Rect(0, 0, border_image.width(), border_image.height()),
                                           layout.border_outer, SkMatrix::kFill_ScaleToFit));
    border_image.draw(canvas);
    canvas.restore();

    auto eye_path = kEyeShape.makeTransform(SkMatrix::Translate(layout.eye_center));

    auto& eye_image = EyeImage();
    canvas.save();
    eye_path.toggleInverseFillType();
    canvas.clipPath(eye_path);
    eye_path.toggleInverseFillType();
    canvas.translate(-eye_image.width() / 2, -eye_image.height() / 2 + layout.eye_center.y);
    eye_image.draw(canvas);
    canvas.restore();

    auto& iris_image = IrisImage();
    {
      canvas.save();
      canvas.clipPath(eye_path);
      canvas.drawColor(SK_ColorWHITE);
      canvas.translate(0, layout.eye_center.y);
      {
        canvas.save();

        Vec2 iris_pos = iris_dir.value * Vec2(2_mm, 1_mm);

        float degrees = atan(iris_pos) * 180 / M_PI;
        canvas.translate(iris_pos.x, iris_pos.y);
        float squeeze_3d = 1 - Length(iris_dir) / 4;
        canvas.rotate(degrees);
        canvas.scale(squeeze_3d, 1);
        canvas.rotate(-degrees);

        canvas.translate(-iris_image.width() / 2, -iris_image.height() / 2);
        iris_image.draw(canvas);
        canvas.restore();
      }
      {
        canvas.save();
        canvas.translate(-eye_image.width() / 2, -eye_image.height() / 2);
        SkPaint paint = eye_image.paint;
        paint.setBlendMode(SkBlendMode::kModulate);
        Rect rect = Rect(0, 0, eye_image.width(), eye_image.height());
        canvas.drawRect(rect, paint);
        canvas.restore();
      }
      canvas.restore();
    }

    if constexpr (kDebugEyeShape) {
      SkPaint eye_paint;
      eye_paint.setStyle(SkPaint::kStroke_Style);
      eye_paint.setColor("#ff0000"_color);
      canvas.drawPath(eye_path, eye_paint);
    }

    if (laser_alpha > 0.0f) {
      SkPath path;
      path.moveTo(layout[Left, Top, Front, Outer]);
      path.lineTo(layout[Right, Top, Front, Outer]);
      path.lineTo(layout[Right, Top, Front, Inner]);
      path.lineTo(layout[Right, Bottom, Front, Inner]);
      path.lineTo(layout[Right, Bottom, Back, Inner]);
      path.lineTo(layout[Left, Bottom, Back, Inner]);
      path.lineTo(layout[Left, Bottom, Back, Outer]);
      path.lineTo(layout[Left, Top, Back, Outer]);
      path.lineTo(layout[Left, Top, Front, Outer]);
      path.lineTo(layout[Left, Top, Front, Inner]);
      path.lineTo(layout[Left, Top, Back, Inner]);
      path.lineTo(layout[Right, Top, Back, Inner]);
      path.lineTo(layout[Right, Top, Back, Outer]);
      path.lineTo(layout[Right, Bottom, Back, Outer]);
      path.lineTo(layout[Right, Bottom, Front, Outer]);
      path.lineTo(layout[Left, Bottom, Front, Outer]);
      path.lineTo(layout[Left, Bottom, Front, Inner]);
      path.lineTo(layout[Left, Bottom, Back, Inner]);
      path.lineTo(layout[Left, Top, Back, Inner]);
      path.lineTo(layout[Left, Top, Back, Outer]);
      path.lineTo(layout[Right, Top, Back, Outer]);
      path.lineTo(layout[Right, Top, Front, Outer]);
      path.lineTo(layout[Right, Bottom, Front, Outer]);
      path.lineTo(layout[Right, Bottom, Front, Inner]);
      path.lineTo(layout[Left, Bottom, Front, Inner]);
      path.lineTo(layout[Left, Top, Front, Inner]);
      path.lineTo(layout[Right, Top, Front, Inner]);
      path.lineTo(layout[Right, Top, Back, Inner]);
      path.lineTo(layout[Right, Bottom, Back, Inner]);
      path.lineTo(layout[Right, Bottom, Back, Outer]);
      path.lineTo(layout[Left, Bottom, Back, Outer]);
      path.lineTo(layout[Left, Bottom, Front, Outer]);
      path.lineTo(layout[Left, Top, Front, Outer]);
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
    WakeAnimation();
  }

  // Forward declaration for drag action
  struct RegionDragAction : Action {
    TesseractWidget& widget;
    DragMode mode;
    Vec2 last_pos;
    Vec2 delta_remainder;

    RegionDragAction(gui::Pointer& pointer, TesseractWidget& widget, DragMode mode)
        : Action(pointer), widget(widget), mode(mode) {
      last_pos = pointer.pointer_position;
    }
    void Update() override {
      auto min_corner = widget.layout[Left, Bottom, Back, Outer];
      auto max_corner = widget.layout[Right, Top, Back, Outer];
      Vec2 size = max_corner - min_corner;
      auto transform = TransformDown(widget);
      Vec2 old_pos = transform.mapPoint(last_pos);
      Vec2 new_pos = transform.mapPoint(pointer.pointer_position);
      last_pos = pointer.pointer_position;
      Vec2 delta = (new_pos - old_pos) / size + delta_remainder;
      if (auto tesseract = widget.LockTesseract()) {
        {
          auto Round = [](float& value, float steps, float* remainder) {
            float rounded = roundf(value * steps) / steps;
            if (remainder) {
              *remainder = value - rounded;
            }
            value = rounded;
          };
          auto lock = std::lock_guard(tesseract->mutex);
          switch (mode) {
            case DragMode::Top: {
              tesseract->y_max_ratio += delta.y;
              if (widget.source_image) {
                Round(tesseract->y_max_ratio, widget.source_image->height(), &delta_remainder.y);
              }
              tesseract->y_max_ratio =
                  std::clamp(tesseract->y_max_ratio, tesseract->y_min_ratio, 1.0f);
              break;
            }
            case DragMode::Bottom:
              tesseract->y_min_ratio += delta.y;
              if (widget.source_image) {
                Round(tesseract->y_min_ratio, widget.source_image->height(), &delta_remainder.y);
              }
              tesseract->y_min_ratio =
                  std::clamp(tesseract->y_min_ratio, 0.0f, tesseract->y_max_ratio);
              break;
            case DragMode::Left:
              tesseract->x_min_ratio += delta.x;
              if (widget.source_image) {
                Round(tesseract->x_min_ratio, widget.source_image->width(), &delta_remainder.x);
              }
              tesseract->x_min_ratio =
                  std::clamp(tesseract->x_min_ratio, 0.0f, tesseract->x_max_ratio);
              break;
            case DragMode::Right:
              tesseract->x_max_ratio += delta.x;
              if (widget.source_image) {
                Round(tesseract->x_max_ratio, widget.source_image->width(), &delta_remainder.x);
              }
              tesseract->x_max_ratio =
                  std::clamp(tesseract->x_max_ratio, tesseract->x_min_ratio, 1.0f);
              break;
            case DragMode::Move:
              if (widget.source_image) {
                delta_remainder = delta;
                auto scale = Vec2(widget.source_image->width(), widget.source_image->height());
                delta = delta * scale;
                delta.x = truncf(delta.x);
                delta.y = truncf(delta.y);
                delta = delta / scale;
                delta_remainder -= delta;
              }
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
      return Vec2AndDir{layout.eye_center, 90_deg};
    }
    if (&arg == &text_arg) {
      return Vec2AndDir{layout.border_outer.LeftCenter(), 180_deg};
    }
    return FallbackWidget::ArgStart(arg);
  }
};

const SkPath TesseractWidget::kEyeShape = PathFromSVG(
    "M-13.6025.7-12.1917-1.6769-9.2847-4.259-6.0956-5.8322-2.0343-6.7556 2.5143-6.5504 "
    "4.9339-6.0203 7.3365-5.2166 8.8584-3.994 10.9103-2.0104 12.1074-.326 12.5263.871 10.9531 "
    "2.3929 9.8245 3.2222 7.6101 4.6757 5.3956 5.4623 3.5744 5.8813 1.2659 6.0694-2.5645 "
    "6.001-4.5481 5.7701-7.3867 5.1033-9.3703 4.0431-11.5847 2.4955-13.0382 1.2985-13.3888.9308Z");

Ptr<gui::Widget> TesseractOCR::MakeWidget() {
  return MakePtr<TesseractWidget>(AcquireWeakPtr<Object>());
}

void TesseractOCR::Args(std::function<void(Argument&)> cb) {
  cb(image_arg);
  cb(text_arg);
  cb(next_arg);
}

void TesseractOCR::OnRun(Location& here, RunTask&) {
  ZoneScopedN("TesseractOCR");
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
      std::unique_ptr<tesseract::ResultIterator> it(tesseract.GetIterator());
      constexpr auto level = tesseract::RIL_TEXTLINE;
      status_results.clear();
      if (it && !it->Empty(level)) {
        do {
          int left, top, right, bottom;
          it->BoundingBox(level, &left, &top, &right, &bottom);
          std::unique_ptr<char[]> text(it->GetUTF8Text(level));
          status_results.push_back({Rect(left, bottom, right, top), text.get()});
          // LOG << "Bounding box: " << left << ", " << top << ", " << right << ", " << bottom << "
          // - "
          //     << text.get();
        } while (it->Next(level));
      }
    }
    std::unique_ptr<char[]> text(tesseract.GetUTF8Text());
    utf8_text = text.get();
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

void TesseractOCR::Updated(Location& here, Location& updated) { WakeWidgetsAnimation(); }

}  // namespace automat::library