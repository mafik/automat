#include "window_impl.h"

#include "font.h"
#include "keyboard_impl.h"
#include "pointer_impl.h"
#include "prototypes.h"
#include "touchpad.h"

namespace automaton::gui {

static SkColor background_color = SkColorSetRGB(0x80, 0x80, 0x80);
static SkColor tick_color = SkColorSetRGB(0x40, 0x40, 0x40);

vec2 RoundToMilimeters(vec2 v) {
  return Vec2(round(v.X * 1000) / 1000., round(v.Y * 1000) / 1000.);
}

struct AnimatedRound : animation::Approach {
  AnimatedRound() : animation::Approach(0) { speed = 50; }
};

struct DragAction : Action {
  std::unique_ptr<Object> object;
  vec2 contact_point;
  vec2 current_position;
  product_ptr<AnimatedRound> round_x;
  product_ptr<AnimatedRound> round_y;

  void Begin(Pointer &pointer) override {
    current_position = pointer.PositionWithin(*root_machine);
  }
  void Update(Pointer &pointer) override {
    auto old_pos = current_position - contact_point;
    auto old_round = RoundToMilimeters(old_pos);
    current_position = pointer.PositionWithin(*root_machine);
    auto new_pos = current_position - contact_point;
    auto new_round = RoundToMilimeters(new_pos);
    if (old_round.X == new_round.X) {
      for (AnimatedRound &rx : round_x) {
        rx.value -= new_pos.X - old_pos.X;
      }
    }
    if (old_round.Y == new_round.Y) {
      for (AnimatedRound &ry : round_y) {
        ry.value -= new_pos.Y - old_pos.Y;
      }
    }
  }
  void End() override {
    RunOnAutomatonThread([pos = current_position - contact_point,
                          obj = object.release()]() mutable {
      Location &loc = root_machine->CreateEmpty();
      loc.position = RoundToMilimeters(pos);
      loc.InsertHere(std::unique_ptr<Object>(obj));
    });
  }
  void Draw(SkCanvas &canvas, animation::State &animation_state) override {
    auto original = current_position - contact_point;
    auto rounded = RoundToMilimeters(original);

    auto &rx = round_x[animation_state];
    auto &ry = round_y[animation_state];
    rx.target = rounded.X - original.X;
    ry.target = rounded.Y - original.Y;
    rx.Tick(animation_state);
    ry.Tick(animation_state);

    canvas.save();
    auto pos = current_position - contact_point + Vec2(rx, ry);
    canvas.translate(pos.X, pos.Y);
    object->Draw(canvas, animation_state);
    canvas.restore();
  }
};

std::unique_ptr<Action> PrototypeButton::ButtonDownAction(Pointer &,
                                                          PointerButton btn,
                                                          vec2 contact_point) {
  if (btn != kMouseLeft) {
    return nullptr;
  }
  auto drag_action = std::make_unique<DragAction>();
  drag_action->object = proto->Clone();
  drag_action->contact_point = contact_point;
  return drag_action;
}

namespace {
std::vector<WindowImpl *> windows;
} // namespace

WindowImpl::WindowImpl(vec2 size, float display_pixels_per_meter)
    : size(size), display_pixels_per_meter(display_pixels_per_meter) {
  prototype_buttons.reserve(Prototypes().size());
  for (auto &proto : Prototypes()) {
    prototype_buttons.emplace_back(this, proto);
    prototype_button_positions.emplace_back(Vec2(0, 0));
  }
  ArrangePrototypeButtons();
  windows.push_back(this);
}

WindowImpl::~WindowImpl() {
  auto it = std::find(windows.begin(), windows.end(), this);
  if (it != windows.end()) {
    windows.erase(it);
  }
}

void WindowImpl::Draw(SkCanvas &canvas) {
  animation_state.timer.Tick();
  RunOnAutomatonThreadSynchronous([&] {
    // Record camera movement timeline. This is used to create inertia effect.
    camera_timeline.emplace_back(Vec3(camera_x, camera_y, zoom));
    timeline.emplace_back(animation_state.timer.now);
    while (timeline.front() < animation_state.timer.now - time::duration(0.2)) {
      camera_timeline.pop_front();
      timeline.pop_front();
    }

    bool panning_now = false;
    vec2 total_pan = Vec2(0, 0);
    float total_zoom = 1;
    {
      std::lock_guard<std::mutex> lock(touchpad::touchpads_mutex);
      for (touchpad::TouchPad *touchpad : touchpad::touchpads) {
        total_pan += touchpad->pan;
        touchpad->pan = Vec2(0, 0);
        total_zoom *= touchpad->zoom;
        touchpad->zoom = 1;
        panning_now |= touchpad->panning;
      }
    }
    if (total_pan != Vec2(0, 0)) {
      camera_x.Shift(total_pan.X / zoom);
      camera_y.Shift(total_pan.Y / zoom);
    }
    if (total_zoom != 1) {
      Zoom(total_zoom);
    }
    if (panning_now) {
      inertia = false;
    }
    if (panning_during_last_frame && !panning_now) {
      // Panning just stopped - apply inertia effect
      inertia = true;
    }
    panning_during_last_frame = panning_now;

    if (inertia) {
      if (timeline.size() > 1) {
        auto dt = (timeline.back() - timeline.front()).count();
        auto dx = camera_timeline.back().X - camera_timeline.front().X;
        auto dy = camera_timeline.back().Y - camera_timeline.front().Y;
        auto dz = camera_timeline.back().Z / camera_timeline.front().Z;
        camera_x.Shift(dx / dt * animation_state.timer.d * 0.8);
        camera_y.Shift(dy / dt * animation_state.timer.d * 0.8);
        float z = pow(dz, animation_state.timer.d / dt * 0.8);
        zoom.target *= z;
        zoom.value *= z;
        float lz = logf(z);
        float dpx = sqrtf(dx * dx + dy * dy + lz * lz) * PxPerMeter();
        if (dpx < 1) {
          inertia = false;
        }
      }
    }

    float rx = camera_x.Remaining();
    float ry = camera_y.Remaining();
    float rz = zoom.Remaining();
    float r = sqrt(rx * rx + ry * ry);
    float rpx = PxPerMeter() * r;
    bool stabilize_mouse = rpx < 1;

    if (stabilize_mouse) {
      if (pointers.size() > 0) {
        PointerImpl *first_pointer = *pointers.begin();
        vec2 mouse_position = first_pointer->pointer_position;
        vec2 focus_pre = WindowToCanvas(mouse_position);
        zoom.Tick(animation_state);
        vec2 focus_post = WindowToCanvas(mouse_position);
        vec2 focus_delta = focus_post - focus_pre;
        camera_x.Shift(-focus_delta.X);
        camera_y.Shift(-focus_delta.Y);
      }
    } else { // stabilize camera target
      vec2 focus_pre = Vec2(camera_x.target, camera_y.target);
      vec2 target_screen = CanvasToWindow(focus_pre);
      zoom.Tick(animation_state);
      vec2 focus_post = WindowToCanvas(target_screen);
      vec2 focus_delta = focus_post - focus_pre;
      camera_x.value -= focus_delta.X;
      camera_y.value -= focus_delta.Y;
    }

    camera_x.Tick(animation_state);
    camera_y.Tick(animation_state);

    for (KeyboardImpl *keyboard : keyboards) {
      if (keyboard->carets.empty()) {
        if (keyboard->pressed_keys.test((size_t)AnsiKey::W)) {
          camera_y.Shift(0.1 * animation_state.timer.d);
          inertia = false;
        }
        if (keyboard->pressed_keys.test((size_t)AnsiKey::S)) {
          camera_y.Shift(-0.1 * animation_state.timer.d);
          inertia = false;
        }
        if (keyboard->pressed_keys.test((size_t)AnsiKey::A)) {
          camera_x.Shift(-0.1 * animation_state.timer.d);
          inertia = false;
        }
        if (keyboard->pressed_keys.test((size_t)AnsiKey::D)) {
          camera_x.Shift(0.1 * animation_state.timer.d);
          inertia = false;
        }
      }
    }

    SkRect work_area = SkRect::MakeXYWH(-0.5, -0.5, 1, 1);

    // Make sure that work area doesn't leave the window bounds (so the user
    // doesn't get lost)
    {
      // Leave 1mm of margin so that the user can still see the edge of the
      // work area
      vec2 bottom_left = WindowToCanvas(Vec2(0.001, 0.001));
      vec2 top_right = WindowToCanvas(size - Vec2(0.001, 0.001));
      SkRect window_bounds = SkRect::MakeLTRB(bottom_left.X, top_right.Y,
                                              top_right.X, bottom_left.Y);
      if (work_area.left() > window_bounds.right()) {
        camera_x.Shift(work_area.left() - window_bounds.right());
      }
      if (work_area.right() < window_bounds.left()) {
        camera_x.Shift(work_area.right() - window_bounds.left());
      }
      // The y axis is flipped so `work_area.bottom()` is actually its top
      if (work_area.bottom() < window_bounds.bottom()) {
        camera_y.Shift(work_area.bottom() - window_bounds.bottom());
      }
      if (work_area.top() > window_bounds.top()) {
        camera_y.Shift(work_area.top() - window_bounds.top());
      }
    }

    canvas.save();
    canvas.translate(size.Width / 2., size.Height / 2.);
    canvas.scale(zoom, zoom);
    canvas.translate(-camera_x, -camera_y);

    // Draw background
    canvas.clear(background_color);
    canvas.drawRect(work_area, GetBackgroundPaint());
    SkPaint border_paint;
    border_paint.setColor(tick_color);
    border_paint.setStyle(SkPaint::kStroke_Style);
    canvas.drawRect(work_area, border_paint);

    // Draw target window size when zooming in with middle mouse button
    if (zoom.target == 1 && rz > 0.001) {
      SkPaint target_paint(SkColor4f(0, 0.3, 0.8, rz));
      target_paint.setStyle(SkPaint::kStroke_Style);
      target_paint.setStrokeWidth(0.001); // 1mm
      float target_width = size.Width;
      float target_height = size.Height;
      SkRect target_rect = SkRect::MakeXYWH(camera_x.target - target_width / 2,
                                            camera_y.target - target_height / 2,
                                            target_width, target_height);
      canvas.drawRect(target_rect, target_paint);
    }

    root_machine->DrawContents(canvas, animation_state);

    for (auto &pointer : pointers) {
      pointer->Draw(canvas, animation_state);
    }

    for (auto &each_window : windows) {
      for (auto &each_keyboard : each_window->keyboards) {
        each_keyboard->Draw(canvas, animation_state);
      }
    }

    canvas.restore();
  }); // RunOnAutomatonThreadSynchronous

  // Draw prototype shelf
  for (int i = 0; i < prototype_buttons.size(); i++) {
    vec2 &position = prototype_button_positions[i];
    canvas.save();
    canvas.translate(position.X, position.Y);
    prototype_buttons[i].Draw(canvas, animation_state);
    canvas.restore();
  }

  // Draw fps counter
  float fps = 1.0f / animation_state.timer.d;
  fps_history.push_back(fps);
  while (fps_history.size() > 100) {
    fps_history.pop_front();
  }
  std::vector<float> fps_sorted(fps_history.begin(), fps_history.end());
  std::sort(fps_sorted.begin(), fps_sorted.end());
  float median_fps = fps_sorted[fps_sorted.size() / 2];
  std::string fps_str = f("FPS min: %3.0f @50%%: %3.0f max: %3.0f",
                          fps_sorted.front(), median_fps, fps_sorted.back());
  SkPaint fps_paint;
  auto &font = GetFont();
  canvas.save();
  canvas.translate(0.001, size.Y - 0.001 - gui::kLetterSize);
  font.DrawText(canvas, fps_str, fps_paint);
  canvas.restore();
}

void WindowImpl::Zoom(float delta) {
  if (pointers.size() > 0) {
    PointerImpl *first_pointer = *pointers.begin();
    vec2 mouse_position = first_pointer->pointer_position;
    vec2 focus_pre = WindowToCanvas(mouse_position);
    zoom.target *= delta;
    zoom.value *= delta;
    vec2 focus_post = WindowToCanvas(mouse_position);
    vec2 focus_delta = focus_post - focus_pre;
    camera_x.Shift(-focus_delta.X);
    camera_y.Shift(-focus_delta.Y);
  } else {
    zoom.target *= delta;
    zoom.value *= delta;
  }
}

} // namespace automaton::gui
