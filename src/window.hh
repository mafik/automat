#pragma once

#include <include/core/SkCanvas.h>

#include "animation.hh"
#include "base.hh"
#include "control_flow.hh"
#include "keyboard.hh"
#include "math.hh"
#include "root.hh"
#include "widget.hh"

namespace automat::gui {

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

struct Keyboard;
struct Pointer;
struct WindowImpl;

struct Window final : Widget {
  Window(Vec2 size, float pixels_per_meter, std::string_view initial_state = "");
  ~Window();

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

  SkPaint& GetBackgroundPaint();

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
  void DisplayPixelDensity(float pixels_per_meter);
  SkPath Shape() const override {
    return SkPath::Rect(SkRect::MakeXYWH(0, 0, size.width, size.height));
  }
  void Draw(SkCanvas&);
  void Draw(gui::DrawContext&) const override {
    FATAL << "Window::Draw(DrawContext&) should never be called";
  }
  void Zoom(float delta);
  ControlFlow VisitChildren(Visitor& visitor) override {
    for (int i = 0; i < prototype_buttons.size(); i++) {
      if (visitor(prototype_buttons[i]) == ControlFlow::Stop) return ControlFlow::Stop;
    }
    ControlFlow result = ControlFlow::Continue;
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

  animation::Context actx;

  std::vector<PrototypeButton> prototype_buttons;
  std::vector<Vec2> prototype_button_positions;

  std::deque<float> fps_history;

  std::vector<Pointer*> pointers;
  std::vector<Keyboard*> keyboards;
};

}  // namespace automat::gui
