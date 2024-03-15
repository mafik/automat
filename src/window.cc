#include "window.hh"

#include <memory>

#include <include/effects/SkRuntimeEffect.h>

#include "drag_action.hh"
#include "font.hh"
#include "prototypes.hh"
#include "touchpad.hh"

namespace automat::gui {

namespace {
std::vector<Window*> windows;
}  // namespace

Window::Window(Vec2 size, float pixels_per_meter, std::string_view initial_state)
    : size(size), display_pixels_per_meter(pixels_per_meter) {
  prototype_buttons.reserve(Prototypes().size());
  for (auto& proto : Prototypes()) {
    prototype_buttons.emplace_back(proto);
    prototype_button_positions.emplace_back(Vec2(0, 0));
  }
  ArrangePrototypeButtons();
  windows.push_back(this);
}
Window::~Window() {
  auto it = std::find(windows.begin(), windows.end(), this);
  if (it != windows.end()) {
    windows.erase(it);
  }
}

static SkColor background_color = SkColorSetRGB(0x80, 0x80, 0x80);
static SkColor tick_color = SkColorSetRGB(0x40, 0x40, 0x40);

std::unique_ptr<Action> PrototypeButton::ButtonDownAction(Pointer& pointer, PointerButton btn) {
  if (btn != kMouseLeft) {
    return nullptr;
  }
  auto drag_action = std::make_unique<DragObjectAction>();
  drag_action->object = proto->Clone();
  drag_action->contact_point = pointer.PositionWithin(*this);
  return drag_action;
}

void Window::Draw(SkCanvas& canvas) {
  actx.timer.Tick();
  gui::DrawContext draw_ctx(canvas, actx);
  draw_ctx.path.push_back(this);
  draw_ctx.path.push_back(root_machine);
  RunOnAutomatThreadSynchronous([&] {
    // Record camera movement timeline. This is used to create inertia effect.
    camera_timeline.emplace_back(Vec3(camera_x, camera_y, zoom));
    timeline.emplace_back(actx.timer.now);
    while (timeline.front() < actx.timer.now - time::duration(0.2)) {
      camera_timeline.pop_front();
      timeline.pop_front();
    }

    bool panning_now = false;
    Vec2 total_pan = Vec2(0, 0);
    float total_zoom = 1;
    {
      std::lock_guard<std::mutex> lock(touchpad::touchpads_mutex);
      for (touchpad::TouchPad* touchpad : touchpad::touchpads) {
        total_pan += touchpad->pan;
        touchpad->pan = Vec2(0, 0);
        total_zoom *= touchpad->zoom;
        touchpad->zoom = 1;
        panning_now |= touchpad->panning;
      }
    }
    if (total_pan != Vec2(0, 0)) {
      camera_x.Shift(total_pan.x / zoom);
      camera_y.Shift(total_pan.y / zoom);
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
        auto dx = camera_timeline.back().x - camera_timeline.front().x;
        auto dy = camera_timeline.back().y - camera_timeline.front().y;
        auto dz = camera_timeline.back().z / camera_timeline.front().z;
        camera_x.Shift(dx / dt * actx.timer.d * 0.8);
        camera_y.Shift(dy / dt * actx.timer.d * 0.8);
        float z = pow(dz, actx.timer.d / dt * 0.8);
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
        Pointer* first_pointer = *pointers.begin();
        Vec2 mouse_position = first_pointer->pointer_position;
        Vec2 focus_pre = WindowToCanvas(mouse_position);
        zoom.Tick(actx);
        Vec2 focus_post = WindowToCanvas(mouse_position);
        Vec2 focus_delta = focus_post - focus_pre;
        camera_x.Shift(-focus_delta.x);
        camera_y.Shift(-focus_delta.y);
      }
    } else {  // stabilize camera target
      Vec2 focus_pre = Vec2(camera_x.target, camera_y.target);
      Vec2 target_screen = CanvasToWindow(focus_pre);
      zoom.Tick(actx);
      Vec2 focus_post = WindowToCanvas(target_screen);
      Vec2 focus_delta = focus_post - focus_pre;
      camera_x.value -= focus_delta.x;
      camera_y.value -= focus_delta.y;
    }

    camera_x.Tick(actx);
    camera_y.Tick(actx);

    for (Keyboard* keyboard : keyboards) {
      if (keyboard->carets.empty()) {
        if (keyboard->pressed_keys.test((size_t)AnsiKey::W)) {
          camera_y.Shift(0.1 * actx.timer.d);
          inertia = false;
        }
        if (keyboard->pressed_keys.test((size_t)AnsiKey::S)) {
          camera_y.Shift(-0.1 * actx.timer.d);
          inertia = false;
        }
        if (keyboard->pressed_keys.test((size_t)AnsiKey::A)) {
          camera_x.Shift(-0.1 * actx.timer.d);
          inertia = false;
        }
        if (keyboard->pressed_keys.test((size_t)AnsiKey::D)) {
          camera_x.Shift(0.1 * actx.timer.d);
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
      Vec2 bottom_left = WindowToCanvas(Vec2(0.001, 0.001));
      Vec2 top_right = WindowToCanvas(size - Vec2(0.001, 0.001));
      SkRect window_bounds =
          SkRect::MakeLTRB(bottom_left.x, top_right.y, top_right.x, bottom_left.y);
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
    canvas.translate(size.width / 2., size.height / 2.);
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
      target_paint.setStrokeWidth(0.001);  // 1mm
      float target_width = size.width;
      float target_height = size.height;
      SkRect target_rect =
          SkRect::MakeXYWH(camera_x.target - target_width / 2, camera_y.target - target_height / 2,
                           target_width, target_height);
      canvas.drawRect(target_rect, target_paint);
    }

    root_machine->DrawChildren(draw_ctx);

    for (auto& pointer : pointers) {
      pointer->Draw(draw_ctx);
    }

    for (auto& each_window : windows) {
      for (auto& each_keyboard : each_window->keyboards) {
        each_keyboard->Draw(draw_ctx);
      }
    }

    canvas.restore();
  });  // RunOnAutomatThreadSynchronous

  draw_ctx.path.pop_back();  // pops root_machine

  // Draw prototype shelf
  for (int i = 0; i < prototype_buttons.size(); i++) {
    Vec2& position = prototype_button_positions[i];
    canvas.save();
    canvas.translate(position.x, position.y);
    prototype_buttons[i].Draw(draw_ctx);
    canvas.restore();
  }

  // Draw fps counter
  float fps = 1.0f / actx.timer.d;
  fps_history.push_back(fps);
  while (fps_history.size() > 100) {
    fps_history.pop_front();
  }
  std::vector<float> fps_sorted(fps_history.begin(), fps_history.end());
  std::sort(fps_sorted.begin(), fps_sorted.end());
  float median_fps = fps_sorted[fps_sorted.size() / 2];
  std::string fps_str = f("FPS min: %3.0f @50%%: %3.0f max: %3.0f", fps_sorted.front(), median_fps,
                          fps_sorted.back());
  SkPaint fps_paint;
  auto& font = GetFont();
  canvas.save();
  canvas.translate(0.001, size.y - 0.001 - gui::kLetterSize);
  font.DrawText(canvas, fps_str, fps_paint);
  canvas.restore();
}

void Window::Zoom(float delta) {
  if (pointers.size() > 0) {
    Pointer* first_pointer = *pointers.begin();
    Vec2 mouse_position = first_pointer->pointer_position;
    Vec2 focus_pre = WindowToCanvas(mouse_position);
    zoom.target *= delta;
    zoom.value *= delta;
    Vec2 focus_post = WindowToCanvas(mouse_position);
    Vec2 focus_delta = focus_post - focus_pre;
    camera_x.Shift(-focus_delta.x);
    camera_y.Shift(-focus_delta.y);
  } else {
    zoom.target *= delta;
    zoom.value *= delta;
  }
}

SkPaint& Window::GetBackgroundPaint() {
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
void Window::DisplayPixelDensity(float pixels_per_meter) {
  display_pixels_per_meter = pixels_per_meter;
}
}  // namespace automat::gui
