#include "window.hh"

#include <include/core/SkPath.h>

#include <memory>

#include "drag_action.hh"
#include "font.hh"
#include "math.hh"
#include "prototypes.hh"
#include "root.hh"
#include "touchpad.hh"

using namespace maf;

namespace automat::gui {

std::vector<Window*> windows;

Window::Window(Vec2 size, float pixels_per_meter)
    : size(size), display_pixels_per_meter(pixels_per_meter) {
  prototype_buttons.reserve(Prototypes().size());
  for (auto& proto : Prototypes()) {
    prototype_buttons.emplace_back(proto);
    prototype_button_positions.emplace_back(Vec2(0, 0));
  }
  ArrangePrototypeButtons();
  windows.push_back(this);
  display.window = this;
}
Window::~Window() {
  auto it = std::find(windows.begin(), windows.end(), this);
  if (it != windows.end()) {
    windows.erase(it);
  }
}

static SkColor background_color = SkColorSetRGB(0x80, 0x80, 0x80);
constexpr float kTrashRadius = 3_cm;

std::unique_ptr<Action> PrototypeButton::ButtonDownAction(Pointer& pointer, PointerButton btn) {
  if (btn != kMouseLeft) {
    return nullptr;
  }
  auto drag_action = std::make_unique<DragObjectAction>(pointer, proto->Clone());
  drag_action->contact_point = pointer.PositionWithin(*this);
  return drag_action;
}

void Window::Draw(SkCanvas& canvas) {
  display.timer.Tick();
  gui::DrawContext draw_ctx(canvas, display);
  draw_ctx.path.push_back(this);
  draw_ctx.path.push_back(root_machine);
  RunOnAutomatThreadSynchronous([&] {
    // Record camera movement timeline. This is used to create inertia effect.
    camera_timeline.emplace_back(Vec3(camera_x, camera_y, zoom));
    timeline.emplace_back(display.timer.now);
    while (timeline.front() < display.timer.now - time::Duration(0.2)) {
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
        camera_x.Shift(dx / dt * display.timer.d * 0.8);
        camera_y.Shift(dy / dt * display.timer.d * 0.8);
        float z = pow(dz, display.timer.d / dt * 0.8);
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
        zoom.Tick(display);
        Vec2 focus_post = WindowToCanvas(mouse_position);
        Vec2 focus_delta = focus_post - focus_pre;
        camera_x.Shift(-focus_delta.x);
        camera_y.Shift(-focus_delta.y);
      }
    } else {  // stabilize camera target
      Vec2 focus_pre = Vec2(camera_x.target, camera_y.target);
      Vec2 target_screen = CanvasToWindow(focus_pre);
      zoom.Tick(display);
      Vec2 focus_post = WindowToCanvas(target_screen);
      Vec2 focus_delta = focus_post - focus_pre;
      camera_x.value -= focus_delta.x;
      camera_y.value -= focus_delta.y;
    }

    camera_x.Tick(display);
    camera_y.Tick(display);

    for (Keyboard* keyboard : keyboards) {
      if (keyboard->carets.empty()) {
        if (keyboard->pressed_keys.test((size_t)AnsiKey::W)) {
          camera_y.Shift(0.1 * display.timer.d);
          inertia = false;
        }
        if (keyboard->pressed_keys.test((size_t)AnsiKey::S)) {
          camera_y.Shift(-0.1 * display.timer.d);
          inertia = false;
        }
        if (keyboard->pressed_keys.test((size_t)AnsiKey::A)) {
          camera_x.Shift(-0.1 * display.timer.d);
          inertia = false;
        }
        if (keyboard->pressed_keys.test((size_t)AnsiKey::D)) {
          camera_x.Shift(0.1 * display.timer.d);
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

    auto default_matrix = canvas.getLocalToDevice();
    canvas.save();
    canvas.concat(CanvasToWindow());

    {  // Animate trash area
      trash_radius.target = drag_action_count ? kTrashRadius : 0;
      trash_radius.Tick(display);
    }

    canvas.clear(background_color);

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

    root_machine->PreDraw(draw_ctx);

    root_machine->Draw(draw_ctx);

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
  float fps = 1.0f / display.timer.d;
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

void Window::DisplayPixelDensity(float pixels_per_meter) {
  display_pixels_per_meter = pixels_per_meter;
}

void Window::SerializeState(Serializer& writer) const {
  writer.StartObject();
  writer.String("width");
  writer.Double(size.width);
  writer.String("height");
  writer.Double(size.height);
  writer.String("camera");
  writer.StartObject();
  writer.String("x");
  writer.Double(camera_x);
  writer.String("y");
  writer.Double(camera_y);
  writer.String("zoom");
  writer.Double(zoom);
  writer.EndObject();
  writer.EndObject();
}

void Window::DeserializeState(Deserializer& d, Status& status) {
  Vec2 new_size = size;
  for (auto& key : ObjectView(d, status)) {
    if (key == "width") {
      new_size.width = d.GetDouble(status);
    } else if (key == "height") {
      new_size.height = d.GetDouble(status);
    } else if (key == "camera") {
      for (auto& camera_key : ObjectView(d, status)) {
        if (camera_key == "x") {
          camera_x = d.GetDouble(status);
        } else if (camera_key == "y") {
          camera_y = d.GetDouble(status);
        } else if (camera_key == "zoom") {
          zoom = d.GetDouble(status);
        } else {
          AppendErrorMessage(status) += f("Unexpected camera key: \"%s\"", camera_key.c_str());
          return;
        }
      }
    } else {
      AppendErrorMessage(status) += f("Unexpected window key: \"%s\"", key.c_str());
      return;
    }
  }
  if (new_size != size && RequestResize) {
    RequestResize(new_size);
  }
}

SkPath Window::TrashShape() const {
  SkPath trash_area_path = SkPath::Circle(size.width, size.height, trash_radius);
  trash_area_path.transform(this->WindowToCanvas());
  return trash_area_path;
}

void Window::SnapPosition(Vec2& position, float& scale, Object* object, Vec2* fixed_point) {
  Rect object_bounds = object->Shape(nullptr).getBounds();
  Rect machine_bounds = root_machine->Shape(nullptr).getBounds();
  Vec2 fake_fixed_point = Vec2(0, 0);
  if (fixed_point == nullptr) {
    fixed_point = &fake_fixed_point;
  }

  float scale1 = 0.5;
  Vec2 position1 = position;
  {  // Find a snap position outside of the canvas
    auto object_bounds_machine = object_bounds.MoveBy(position);
    SkMatrix machine_scale_mat_fixed =
        SkMatrix::Translate(-position)
            .postScale(scale1, scale1, fixed_point->x, fixed_point->y)
            .postTranslate(position.x, position.y);

    Rect scaled_object_bounds = machine_scale_mat_fixed.mapRect(object_bounds_machine);
    Vec2 true_object_origin = machine_scale_mat_fixed.mapPoint(position);
    if (machine_bounds.sk.intersects(scaled_object_bounds)) {
      float move_up = fabsf(machine_bounds.top - scaled_object_bounds.bottom);
      float move_down = fabsf(scaled_object_bounds.top - machine_bounds.bottom);
      float move_left = fabsf(machine_bounds.left - scaled_object_bounds.right);
      float move_right = fabsf(scaled_object_bounds.left - machine_bounds.right);
      if (move_up < move_down && move_up < move_left && move_up < move_right) {
        true_object_origin.y += move_up;
        scaled_object_bounds = scaled_object_bounds.MoveBy({0, move_up});
      } else if (move_down < move_up && move_down < move_left && move_down < move_right) {
        true_object_origin.y -= move_down;
        scaled_object_bounds = scaled_object_bounds.MoveBy({0, -move_down});
      } else if (move_left < move_up && move_left < move_down && move_left < move_right) {
        true_object_origin.x -= move_left;
        scaled_object_bounds = scaled_object_bounds.MoveBy({-move_left, 0});
      } else {
        true_object_origin.x += move_right;
        scaled_object_bounds = scaled_object_bounds.MoveBy({move_right, 0});
      }
    }
    position1 =
        (true_object_origin - scaled_object_bounds.Center()) * 2 + scaled_object_bounds.Center();
  }

  Vec2 window_pos = (position - Vec2(camera_x, camera_y)) * zoom + size / 2;
  bool is_over_trash =
      LengthSquared(window_pos - Vec2(size.width, size.height)) < trash_radius * trash_radius;
  Vec2 box_size = Vec2(object_bounds.Width(), object_bounds.Height());
  float diagonal = Length(box_size);
  SkMatrix mat = WindowToCanvas();
  Vec2 position2 =
      mat.mapPoint(size - box_size / diagonal * trash_radius / 2) - object_bounds.Center();
  float scale2 = mat.mapRadius(trash_radius) / diagonal * 0.9f;
  scale2 = std::clamp<float>(scale2, 0.1, 0.5);
  if (LengthSquared(position1 - position) < LengthSquared(position2 - position)) {
    position = position1;
    scale = scale1;
  } else {
    position = position2;
    scale = scale2;
  }
}

void Window::DropObject(
    std::unique_ptr<Object>&& object, Vec2 position, float scale,
    std::unique_ptr<animation::PerDisplay<ObjectAnimationState>>&& animation_state) {
  // do nothing - objects immediately disappears when dropped on the window
}

void Window::DropLocation(Location* location) {
  auto location_ptr = location->ParentAs<Machine>()->Extract(*location);
}

}  // namespace automat::gui
