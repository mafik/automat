#pragma once

#include <include/core/SkCanvas.h>

#include "animation.hh"
#include "base.hh"
#include "control_flow.hh"
#include "deserializer.hh"
#include "drag_action.hh"
#include "keyboard.hh"
#include "math.hh"
#include "root.hh"
#include "time.hh"
#include "widget.hh"

namespace automat::gui {

// Ensures that the 1x1m canvas is at least 1mm on screen.
constexpr float kMinZoom = 0.001f;
constexpr time::Duration kClickTimeout = 300ms;
constexpr float kClickRadius = 2_mm;

struct PrototypeButton : Widget {
  const Object* proto;
  PrototypeButton(const Object* proto) : proto(proto) {}
  void Draw(DrawContext& ctx) const override { proto->Draw(ctx); }
  SkPath Shape(animation::Display*) const override { return proto->Shape(nullptr); }

  void PointerOver(Pointer& pointer, animation::Display&) override {
    pointer.PushIcon(Pointer::kIconHand);
  }

  void PointerLeave(Pointer& pointer, animation::Display&) override { pointer.PopIcon(); }

  std::unique_ptr<Action> ButtonDownAction(Pointer&, PointerButton btn) override;
};

struct Keyboard;
struct Pointer;
struct WindowImpl;

extern std::vector<Window*> windows;

struct Window final : Widget, DropTarget {
  Window(Vec2 size, float pixels_per_meter);
  ~Window();
  std::string_view Name() const override { return "Window"; }

  void ArrangePrototypeButtons() {
    float max_w = size.width;
    Vec2 cursor = Vec2(0, 0);
    for (int i = 0; i < prototype_buttons.size(); i++) {
      auto& btn = prototype_buttons[i];
      Vec2& pos = prototype_button_positions[i];
      SkPath shape = btn.Shape(nullptr);
      SkRect bounds = shape.getBounds();
      if (cursor.x + bounds.width() + 0.001 > max_w) {
        cursor.x = 0;
        cursor.y += bounds.height() + 0.001;
      }
      pos = cursor + Vec2(0.001, 0.001) - Vec2(bounds.left(), bounds.top());
      cursor.x += bounds.width() + 0.001;
    }
  }

  DropTarget* CanDrop() override { return this; }
  void SnapPosition(Vec2& position, float& scale, Object* object, Vec2* fixed_point) override;
  void DropObject(
      std::unique_ptr<Object>&& object, Vec2 position, float scale,
      std::unique_ptr<animation::PerDisplay<ObjectAnimationState>>&& animation_state) override;
  void DropLocation(Location* location) override;

  // Return the shape of the trash zone in the corner of the window (in Machine coordinates).
  SkPath TrashShape() const;

  float PxPerMeter() { return display_pixels_per_meter * zoom; }

  SkRect GetCameraRect() {
    return SkRect::MakeXYWH(camera_x - size.width / 2, camera_y - size.height / 2, size.width,
                            size.height);
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
    // assert(WindowToCanvas().invert(&m));
    m.setTranslate(-camera_x, -camera_y);
    m.postScale(zoom, zoom);
    m.postTranslate(size.width / 2, size.height / 2);
    return m;
  }

  Vec2 CanvasToWindow(Vec2 canvas) { return (canvas - Vec2(camera_x, camera_y)) * zoom + size / 2; }

  std::function<void(Vec2 new_size)> RequestResize = nullptr;

  void Resize(Vec2 size) {
    this->size = size;
    ArrangePrototypeButtons();
  }
  void DisplayPixelDensity(float pixels_per_meter);
  SkPath Shape(animation::Display*) const override {
    return SkPath::Rect(SkRect::MakeXYWH(0, 0, size.width, size.height));
  }
  void Draw(SkCanvas&);
  void Draw(gui::DrawContext&) const override {
    FATAL << "Window::Draw(DrawContext&) should never be called";
  }
  void Zoom(float delta);
  ControlFlow VisitChildren(Visitor& visitor) override {
    Widget* arr[prototype_buttons.size()];
    for (int i = 0; i < prototype_buttons.size(); i++) {
      arr[i] = &prototype_buttons[i];
    }
    if (visitor(maf::SpanOfArr(arr, prototype_buttons.size())) == ControlFlow::Stop)
      return ControlFlow::Stop;
    ControlFlow result = ControlFlow::Continue;
    RunOnAutomatThreadSynchronous([&]() {
      Widget* root_machine_widget = root_machine;
      result = visitor(maf::Span<Widget*>(&root_machine_widget, 1));
    });
    return result;
  }
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override {
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

  // Called when closing Automat to persist state across restarts.
  void SerializeState(Serializer&) const;

  // Restores state when Automat is restarted.
  void DeserializeState(Deserializer&, maf::Status&);

  Vec2 size;
  float display_pixels_per_meter = 96 / kMetersPerInch;  // default value assumes 96 DPI

  animation::Approach<> zoom = animation::Approach<>(1.0);
  animation::Approach<> camera_x = animation::Approach<>(0.0);
  animation::Approach<> camera_y = animation::Approach<>(0.0);
  animation::Approach<> trash_radius = animation::Approach<>(0.0);
  int drag_action_count = 0;
  bool panning_during_last_frame = false;
  bool inertia = false;
  std::deque<Vec3> camera_timeline;
  std::deque<time::SystemPoint> timeline;

  animation::Display display;

  std::vector<PrototypeButton> prototype_buttons;
  std::vector<Vec2> prototype_button_positions;

  std::deque<float> fps_history;

  std::vector<Pointer*> pointers;
  std::vector<Keyboard*> keyboards;
};

// Converts a point in the screen pixel coordinates (origin at the top left) to window coordinates
// (SI units origin at the bottom left).
Vec2 ScreenToWindow(Vec2 screen);

// Converts a point in the window (SI units origin at the bottom left) to screen pixel coordinates
// (origin at the top left).
Vec2 WindowToScreen(Vec2 window);

// Returns the position of the main pointer in screen coordinates (pixels originating at the top
// left).
Vec2 GetMainPointerScreenPos();

}  // namespace automat::gui
