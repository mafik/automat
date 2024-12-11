// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkCanvas.h>

#include <cmath>

#include "animation.hh"
#include "base.hh"
#include "deserializer.hh"
#include "drag_action.hh"
#include "keyboard.hh"
#include "library_toolbar.hh"
#include "math.hh"
#include "optional.hh"
#include "time.hh"
#include "widget.hh"

namespace automat::gui {

// Ensures that the 1x1m canvas is at least 1mm on screen.
constexpr float kMinZoom = 0.001f;
constexpr time::Duration kClickTimeout = 300ms;
constexpr float kClickRadius = 2_mm;

const char kWindowName[] = "Automat";

struct Keyboard;
struct Pointer;

extern std::vector<Window*> windows;
extern std::shared_ptr<Window> window;

// Objects can create many widgets, to display themselves simultaneously in multiple contexts.
// Each context which can display widgets must maintain their lifetime. This class helps with that.
// It can be used either as a mixin or as a member.
// TODO: introduce `WidgetMaker` interface, so that widgets can be created for non-Objects
// TODO: delete widgets after some time
struct WidgetStore {
  struct WeakPtrCmp {
    bool operator()(const std::weak_ptr<Object>& a, const std::weak_ptr<Object>& b) const {
      return a.owner_before(b);
    }
  };
  std::map<std::weak_ptr<Object>, std::weak_ptr<Widget>, WeakPtrCmp> container;

  std::shared_ptr<Widget> For(Object& object, const Widget& parent) {
    auto weak = object.WeakPtr();
    auto it = container.find(weak);
    if (it == container.end()) {
      auto widget = object.MakeWidget();
      widget->parent = parent.SharedPtr();
      container.emplace(weak, widget);
      return widget;
    } else if (auto strong_ref = it->second.lock()) {
      return strong_ref;
    } else {
      auto widget = object.MakeWidget();
      widget->parent = parent.SharedPtr();
      it->second = widget;
      return widget;
    }
  }
};

struct OSWindow {
  Window& root;
  Vec2 vk_size = Vec2(-1, -1);
  int client_width;  // pixels
  int client_height;
  int screen_refresh_rate = 60;
  std::unique_ptr<gui::Pointer> mouse;

  virtual ~OSWindow() = default;

  virtual void MainLoop() = 0;

  virtual gui::Pointer& GetMouse() = 0;

  // Converts a point in the screen pixel coordinates (origin at the top left) to window pixel
  // coordinates (origin at the bottom left).
  virtual Vec2 ScreenToWindowPx(Vec2 screen) = 0;

  // Converts a point in the window pixel coordinates (origin at the bottom left) to screen pixel
  // coordinates (origin at the top left).
  virtual Vec2 WindowPxToScreen(Vec2 window) = 0;

  virtual maf::Optional<Vec2> MousePositionScreenPx() = 0;

  virtual void RequestResize(Vec2 new_size) = 0;
  virtual void RequestMaximize(bool maximize_horizontally, bool maximize_vertically) = 0;

  std::lock_guard<std::mutex> Lock();

 protected:
  OSWindow(Window& root) : root(root) {}

  // TODO: keep reference to vk::Surface and vk::Swapchain here
};

struct Window final : Widget, DropTarget {
  Window();
  ~Window();

  std::unique_ptr<OSWindow> os_window;

  void InitToolbar();

  WidgetStore widgets;

  std::string_view Name() const override { return "Window"; }

  DropTarget* CanDrop() override { return this; }
  void SnapPosition(Vec2& position, float& scale, Location&, Vec2* fixed_point) override;
  void DropLocation(std::shared_ptr<Location>&&) override;

  // Return the shape of the trash zone in the corner of the window (in Machine coordinates).
  SkPath TrashShape() const;

  float PxPerMeter() const { return display_pixels_per_meter * zoom; }

  SkRect GetCameraRect() {
    return SkRect::MakeXYWH(camera_pos.x - size.width / 2, camera_pos.y - size.height / 2,
                            size.width, size.height);
  }

  SkMatrix WindowToCanvas() const {
    auto m = CanvasToWindow();
    SkMatrix inv;
    (void)m.invert(&inv);
    return inv;
  }

  SkMatrix CanvasToWindow() const {
    SkMatrix m;
    m.setTranslate(-camera_pos.x, -camera_pos.y);
    m.postScale(zoom, zoom);
    m.postTranslate(size.width / 2, size.height / 2);
    return m;
  }

  // Used to tell the window that it's OS window has been resized.
  // Should call Window::Resized() if successful.
  void Resized(Vec2 size);

  // Used to tell the window that it's OS window has been maximized.
  // Should call Window::Maximized() if successful.
  void Maximized(bool horizontally, bool vertically) {
    maximized_horizontally = horizontally;
    maximized_vertically = vertically;
  }

  void DisplayPixelDensity(float pixels_per_meter);
  SkPath Shape() const override {
    return SkPath::Rect(SkRect::MakeXYWH(0, 0, size.width, size.height));
  }
  void Draw(SkCanvas&);

  animation::Phase Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;

  Vec2 move_velocity = Vec2(0, 0);
  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) override;

  void Zoom(float delta);
  void FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) override;
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

  // TODO: Remove (use os_window.px_per_meter instead)
  float display_pixels_per_meter = 96 / kMetersPerInch;  // default value assumes 96 DPI
  std::shared_ptr<Toolbar> toolbar;
  std::vector<std::shared_ptr<gui::ConnectionWidget>> connection_widgets;

  float zoom = 1;
  float zoom_target = 1;
  Vec2 camera_pos = Vec2(0, 0);
  Vec2 camera_target = Vec2(0, 0);
  float trash_radius = 0;
  float trash_radius_target = 0;
  int drag_action_count = 0;
  bool panning_during_last_frame = false;
  bool inertia = false;
  std::deque<Vec3> camera_timeline;
  std::deque<time::SteadyPoint> timeline;

  // `timer` should be advanced once per frame on the device that displays the animation. Its `d`
  // field can be used by animated objects to animate their properties.
  time::Timer timer;

  std::deque<float> fps_history;

  std::vector<Pointer*> pointers;
  std::vector<std::shared_ptr<Keyboard>> keyboards;

  std::mutex mutex;
};

}  // namespace automat::gui
