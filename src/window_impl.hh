#pragma once

#include <include/effects/SkRuntimeEffect.h>

#include <bitset>

#include "base.hh"
#include "root.hh"
#include "widget.hh"

namespace automat::gui {

struct PointerImpl;

// Ensures that the 1x1m canvas is at least 1mm on screen.
constexpr float kMinZoom = 0.001f;
constexpr time::duration kClickTimeout = std::chrono::milliseconds(300);
constexpr float kClickRadius = 0.002f;  // 2mm

struct PrototypeButton : Widget {
  const Object* proto;
  PrototypeButton(const Object* proto) : proto(proto) {}
  void Draw(DrawContext& ctx) const override { proto->Draw(ctx); }
  SkPath Shape() const override { return proto->Shape(); }

  void PointerOver(Pointer& pointer, animation::Context&) override {
    pointer.PushIcon(Pointer::kIconHand);
  }

  void PointerLeave(Pointer& pointer, animation::Context&) override { pointer.PopIcon(); }

  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton btn) override;
};

struct WindowImpl : Widget {
  Vec2 position = Vec2(0, 0);  // center of the window
  Vec2 size;
  float display_pixels_per_meter = 96 / kMetersPerInch;  // default value assumes 96 DPI

  animation::Approach zoom = animation::Approach(1.0, 0.01);
  animation::Approach camera_x = animation::Approach(0.0, 0.005);
  animation::Approach camera_y = animation::Approach(0.0, 0.005);
  bool panning_during_last_frame = false;
  bool inertia = false;
  std::deque<Vec3> camera_timeline;
  std::deque<time::point> timeline;

  std::vector<PointerImpl*> pointers;
  std::vector<KeyboardImpl*> keyboards;

  animation::Context actx;

  std::vector<PrototypeButton> prototype_buttons;
  std::vector<Vec2> prototype_button_positions;

  std::deque<float> fps_history;

  WindowImpl(Vec2 size, float display_pixels_per_meter);

  ~WindowImpl();

  void ArrangePrototypeButtons() {
    float max_w = size.width;
    Vec2 cursor = Vec2(0, 0);
    for (int i = 0; i < prototype_buttons.size(); i++) {
      auto& btn = prototype_buttons[i];
      Vec2& pos = prototype_button_positions[i];
      SkPath shape = btn.Shape();
      SkRect bounds = shape.getBounds();
      if (cursor.x + bounds.width() + 0.001 > max_w) {
        cursor.x = 0;
        cursor.y += bounds.height() + 0.001;
      }
      pos = cursor + Vec2(0.001, 0.001) - Vec2(bounds.left(), bounds.top());
      cursor.x += bounds.width() + 0.001;
    }
  }

  float PxPerMeter() { return display_pixels_per_meter * zoom; }

  SkRect GetCameraRect() {
    return SkRect::MakeXYWH(camera_x - size.width / 2, camera_y - size.height / 2, size.width,
                            size.height);
  }

  SkPaint& GetBackgroundPaint() {
    static SkRuntimeShaderBuilder builder = []() {
      const char* sksl = R"(
        uniform float px_per_m;

        // Dark theme
        //float4 bg = float4(0.05, 0.05, 0.00, 1);
        //float4 fg = float4(0.0, 0.32, 0.8, 1);

        float4 bg = float4(0.9, 0.9, 0.9, 1);
        float4 fg = float4(0.5, 0.5, 0.5, 1);

        float grid(vec2 coord_m, float dots_per_m, float r_px) {
          float r = r_px / px_per_m;
          vec2 grid_coord = fract(coord_m * dots_per_m + 0.5) - 0.5;
          return smoothstep(r, r - 1/px_per_m, length(grid_coord) / dots_per_m) * smoothstep(1./(3*r), 1./(32*r), dots_per_m);
        }

        half4 main(vec2 fragcoord) {
          float dm_grid = grid(fragcoord, 10, 2);
          float cm_grid = grid(fragcoord, 100, 2) * 0.8;
          float mm_grid = grid(fragcoord, 1000, 1) * 0.8;
          float d = max(max(mm_grid, cm_grid), dm_grid);
          return mix(bg, fg, d);
        }
      )";

      auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(sksl));
      if (!err.isEmpty()) {
        FATAL << err.c_str();
      }
      SkRuntimeShaderBuilder builder(effect);
      return builder;
    }();
    static SkPaint paint;
    builder.uniform("px_per_m") = PxPerMeter();
    paint.setShader(builder.makeShader());
    return paint;
  }

  Vec2 WindowToCanvas(Vec2 window) const {
    return (window - size / 2) / zoom + Vec2(camera_x, camera_y);
  }

  SkMatrix WindowToCanvas() const {
    SkMatrix m;
    m.setTranslate(-size.width / 2, -size.height / 2);
    m.postScale(1 / zoom, 1 / zoom);
    m.postTranslate(camera_x, camera_y);
    return m;
  }

  SkMatrix CanvasToWindow() const {
    SkMatrix m;
    m.setTranslate(-camera_x, -camera_y);
    m.postScale(zoom, zoom);
    m.postTranslate(size.width / 2, size.height / 2);
    return m;
  }

  Vec2 CanvasToWindow(Vec2 canvas) { return (canvas - Vec2(camera_x, camera_y)) * zoom + size / 2; }

  void Resize(Vec2 size) {
    this->size = size;
    ArrangePrototypeButtons();
  }
  void DisplayPixelDensity(float pixels_per_meter) {
    this->display_pixels_per_meter = pixels_per_meter;
  }
  SkPath Shape() const override {
    return SkPath::Rect(SkRect::MakeXYWH(0, 0, size.width, size.height));
  }
  void Draw(gui::DrawContext&) const override {
    FATAL << "WindowImpl::Draw() should never be called";
  }
  void Draw(SkCanvas& canvas);
  void Zoom(float delta);
  MaybeStop VisitChildren(Visitor& visitor) override {
    for (int i = 0; i < prototype_buttons.size(); i++) {
      if (auto stop = visitor(prototype_buttons[i])) return stop;
    }
    MaybeStop result = std::nullopt;
    RunOnAutomatThreadSynchronous([&]() { result = visitor(*root_machine); });
    return result;
  }
  SkMatrix TransformToChild(const Widget& child, animation::Context&) const override {
    for (int i = 0; i < prototype_buttons.size(); i++) {
      if (&child == &prototype_buttons[i]) {
        return SkMatrix::Translate(-prototype_button_positions[i].x,
                                   -prototype_button_positions[i].y);
      }
    }
    if (&child == root_machine) {
      return WindowToCanvas();
    }
    return SkMatrix::I();
  }
  std::unique_ptr<Pointer> MakePointer(Vec2 position);
  std::string_view GetState();
};

}  // namespace automat::gui
