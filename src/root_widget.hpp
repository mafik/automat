#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <include/core/SkCanvas.h>

#include <cmath>
#include <thread>

#include "animation.hpp"
#include "base.hpp"
#include "black_hole.hpp"
#include "concurrent_bool.hpp"
#include "deserializer.hpp"
#include "drag_action.hpp"
#include "keyboard.hpp"
#include "library_toolbar.hpp"
#include "loading_animation.hpp"
#include "math.hpp"
#include "mortal.hpp"
#include "time.hpp"
#include "widget.hpp"
#include "window.hpp"

namespace automat::ui {

// Ensures that the 1x1m canvas is at least 1mm on screen.
constexpr float kMinZoom = 0.001f;
constexpr time::Duration kClickTimeout = 300ms;
constexpr float kClickRadius = 2_mm;

const char kWindowName[] = "Automat";

struct Keyboard;
struct Pointer;

extern std::vector<RootWidget*> root_widgets;
extern unique_ptr<RootWidget> root_widget;

struct Camera : Object {
  std::mutex mutex;
  Vec2 nudge;

  StrView Name() const override { return "Camera"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Camera); }

  void Move(Vec2 delta) {
    {
      auto lock = std::lock_guard(mutex);
      nudge += delta;
    }
    WakeToys();
  }

  DEF_INTERFACE(Camera, Signal, up, "Camera up")
  static constexpr bool kSchedulesNext = false;
  static constexpr Vec2 kDelta = Vec2(0, 10_cm);
  void OnRun(std::unique_ptr<RunTask>&) { obj->Move(kDelta); }
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(up);
  DEF_INTERFACE(Camera, Signal, down, "Camera down")
  static constexpr bool kSchedulesNext = false;
  static constexpr Vec2 kDelta = Vec2(0, -10_cm);
  void OnRun(std::unique_ptr<RunTask>&) { obj->Move(kDelta); }
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(down);
  DEF_INTERFACE(Camera, Signal, left, "Camera left")
  static constexpr bool kSchedulesNext = false;
  static constexpr Vec2 kDelta = Vec2(-10_cm, 0);
  void OnRun(std::unique_ptr<RunTask>&) { obj->Move(kDelta); }
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(left);
  DEF_INTERFACE(Camera, Signal, right, "Camera right")
  static constexpr bool kSchedulesNext = false;
  static constexpr Vec2 kDelta = Vec2(10_cm, 0);
  void OnRun(std::unique_ptr<RunTask>&) { obj->Move(kDelta); }
  std::unique_ptr<Action> OnActivate(ui::Pointer&, automat::Toy*);
  DEF_END(right);

  INTERFACES(up, down, left, right)
};

extern Signal::Table kDragCamera;
extern Signal::Table kCameraMenu;

struct RootWidget final : Widget {
  RootWidget();
  ~RootWidget();

  // true when Wayland windows should receive input before Automat
  constexpr static bool kWaylandLock = false;

  std::jthread render_thread;

  std::unique_ptr<Window> window;
  std::unique_ptr<LoadingAnimation> loading_animation;

  struct ZoomWarning : Widget {
    float zoom_limit_alpha = 0;
    float zoom_limit_scroll = 0;
    RootWidget* root_widget;

    ZoomWarning(RootWidget* root_widget) : Widget(root_widget), root_widget(root_widget) {
      root_widget->layers.OrderInside(this);
    }
    SkPath Shape() const override { return SkPath(); }
    Optional<Rect> DrawBounds() const override { return std::nullopt; }
    Tock Tick(time::Timer&) override;
    void Draw(SkCanvas&) const override;
  } zoom_warning;

  BlackHole black_hole;

  void Init();

  struct ToyStore toys;
  MortalList<Action> active_actions;

  std::string_view Name() const override { return "RootWidget"; }

  void Poll();

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

  SkMatrix PointerToCanvas() const {
    auto px2canvas = TransformDown(*this);
    px2canvas.postConcat(WindowToCanvas());
    return px2canvas;
  }

  ConcurrentBool minimized;

  // Tells Automat to hide its window, stop rendering & release all GPU resources.
  //
  // Warning: This may be called on non-main thread.
  void MinimizeToTray();

  void RestoreFromTray();

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

  Tock Tick(time::Timer&) override;
  void Draw(SkCanvas&) const override;
  Compositor GetCompositor() const override { return Compositor::WARP; }

  Vec2 move_velocity = Vec2(0, 0);
  Ptr<Camera> camera = MAKE_PTR(Camera);
  uint32_t camera_observed = 0;
  Interface FindOption(Pointer&, ActionTrigger) override;
  MiniMenuMode MenuMode() override { return MODE_4_DIR; }

  void Zoom(float delta);
  std::unique_ptr<Pointer> MakePointer(Vec2 position);

  // Called when closing Automat to persist state across restarts.
  void SerializeState(Serializer&) const;

  // Restores state when Automat is restarted.
  void DeserializeState(Deserializer&, Status&);

  Vec2 size = Vec2(10_cm, 10_cm);
  bool maximized_vertically = false;
  bool maximized_horizontally = false;
  bool always_on_top = false;

  // Position where Automat window should be restored.
  float output_device_x =
      NAN;  // distance from the left edge of the screen (or right when negative)
  float output_device_y =
      NAN;  // distance from the top edge of the screen (or bottom when negative)

  // TODO: Remove (use window.px_per_meter instead)
  float display_pixels_per_meter = 96 / kMetersPerInch;  // default value assumes 96 DPI
  unique_ptr<Toolbar> toolbar;

  float zoom = 1;
  float zoom_target = 1;
  Vec2 camera_pos = Vec2(0, 0);
  Vec2 camera_target = Vec2(0, 0);
  float trash_radius = 0;
  int drag_action_count = 0;
  bool panning_during_last_frame = false;
  bool inertia = false;
  std::deque<Vec3> camera_timeline;
  std::deque<time::SteadyPoint> timeline;

  // `timer` should be advanced once per frame on the device that displays the animation. Its `d`
  // field can be used by animated objects to animate their properties.
  time::Timer timer;

  std::deque<float> fps_history;

  MortalList<Pointer> pointers;
  KeyboardWidget keyboard;

  uint32_t observed_vm_wake_counter = 0;

  std::mutex mutex;
};

}  // namespace automat::ui
