#pragma once

#include <include/core/SkCanvas.h>

#include <cmath>

#include "animation.hh"
#include "base.hh"
#include "control_flow.hh"
#include "deserializer.hh"
#include "drag_action.hh"
#include "keyboard.hh"
#include "library_toolbar.hh"
#include "math.hh"
#include "time.hh"
#include "widget.hh"

namespace automat::gui {

// Ensures that the 1x1m canvas is at least 1mm on screen.
constexpr float kMinZoom = 0.001f;
constexpr time::Duration kClickTimeout = 300ms;
constexpr float kClickRadius = 2_mm;

struct Keyboard;
struct Pointer;
struct WindowImpl;

extern std::vector<Window*> windows;
extern std::unique_ptr<Window> window;

struct Window final : Widget, DropTarget {
  Window();
  ~Window();

  DrawCache draw_cache;
  std::string_view Name() const override { return "Window"; }

  DropTarget* CanDrop() override { return this; }
  void SnapPosition(Vec2& position, float& scale, Object* object, Vec2* fixed_point) override;
  void DropLocation(std::unique_ptr<Location>&&) override;

  // Return the shape of the trash zone in the corner of the window (in Machine coordinates).
  SkPath TrashShape() const;

  float PxPerMeter() const { return display_pixels_per_meter * zoom; }

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

  Vec2 CanvasToWindow(Vec2 canvas) const {
    return (canvas - Vec2(camera_x, camera_y)) * zoom + size / 2;
  }

  std::function<void(Vec2 new_size)> RequestResize = nullptr;
  std::function<void(bool maximize_horizontally, bool maximize_vertically)> RequestMaximize =
      nullptr;

  // Used to tell the window that it's OS window has been resized.
  void Resize(Vec2 size) { this->size = size; }
  void DisplayPixelDensity(float pixels_per_meter);
  SkPath Shape(animation::Display*) const override {
    return SkPath::Rect(SkRect::MakeXYWH(0, 0, size.width, size.height));
  }
  void Draw(SkCanvas&);
  animation::Phase Draw(gui::DrawContext&) const override;
  void Zoom(float delta) const;
  ControlFlow VisitChildren(Visitor& visitor) override;
  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override {
    if (&child == &toolbar) {
      return SkMatrix::Translate(-size.x / 2, 0);
    }
    return WindowToCanvas();
  }
  std::unique_ptr<Pointer> MakePointer(Vec2 position);

  // Called when closing Automat to persist state across restarts.
  void SerializeState(Serializer&) const;

  // Restores state when Automat is restarted.
  void DeserializeState(Deserializer&, maf::Status&);

  Vec2 size = Vec2(10_cm, 10_cm);
  bool maximized_vertically = false;
  bool maximized_horizontally = false;
  bool always_on_top = false;

  // Position where Automat window should be restored.
  float output_device_x =
      NAN;  // distance from the left edge of the screen (or right when negative)
  float output_device_y =
      NAN;  // distance from the top edge of the screen (or bottom when negative)

  float display_pixels_per_meter = 96 / kMetersPerInch;  // default value assumes 96 DPI
  library::Toolbar toolbar;
  std::vector<std::unique_ptr<gui::ConnectionWidget>> connection_widgets;

  mutable float zoom = 1;
  mutable float zoom_target = 1;
  mutable animation::Approach<> camera_x = animation::Approach<>(0.0);
  mutable animation::Approach<> camera_y = animation::Approach<>(0.0);
  mutable animation::Approach<> trash_radius = animation::Approach<>(0.0);
  int drag_action_count = 0;
  mutable bool panning_during_last_frame = false;
  mutable bool inertia = false;
  mutable std::deque<Vec3> camera_timeline;
  mutable std::deque<time::SystemPoint> timeline;

  mutable animation::Display display;

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
