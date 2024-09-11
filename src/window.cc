#include "window.hh"

#include <include/core/SkPath.h>

#include <memory>

#include "drag_action.hh"
#include "font.hh"
#include "gui_connection_widget.hh"
#include "math.hh"
#include "pointer.hh"
#include "prototypes.hh"
#include "root.hh"
#include "touchpad.hh"

using namespace maf;

namespace automat::gui {

std::vector<Window*> windows;
std::unique_ptr<Window> window;

Window::Window() {
  for (auto& proto : Prototypes()) {
    toolbar.AddObjectPrototype(proto);
  }
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

void Window::Draw(SkCanvas& canvas) {
  display.timer.Tick();
  gui::DrawContext draw_ctx(display, canvas, draw_cache);
  draw_ctx.path.push_back(this);
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

    auto window_space_matrix = canvas.getLocalToDevice();
    canvas.save();
    canvas.concat(CanvasToWindow());
    auto machine_space_matrix = canvas.getLocalToDevice();

    {  // Animate trash area
      trash_radius.target = drag_action_count ? kTrashRadius : 0;
      trash_radius.Tick(display);
    }

    canvas.clear(background_color);

    canvas.setMatrix(window_space_matrix);
    auto phase = DrawChildren(draw_ctx);

    canvas.setMatrix(machine_space_matrix);

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

    for (auto& each_window : windows) {
      for (auto& each_keyboard : each_window->keyboards) {
        each_keyboard->Draw(draw_ctx);
      }
    }

    if (phase == animation::Animating) {
      for (auto& each_window : windows) {
        for (auto& each_pointer : each_window->pointers) {
          each_pointer->UpdatePath();
        }
      }
    }

    canvas.restore();
  });  // RunOnAutomatThreadSynchronous

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
  writer.Key("maximized");

  writer.StartObject();
  writer.Key("horizontally");
  writer.Bool(maximized_horizontally);
  writer.Key("vertically");
  writer.Bool(maximized_vertically);
  writer.EndObject();
  if (!isnan(output_device_x)) {
    writer.String("output_device_x");
    writer.Double(output_device_x);
  }
  if (!isnan(output_device_y)) {
    writer.String("output_device_y");
    writer.Double(output_device_y);
  }
  if (always_on_top) {
    writer.String("always_on_top");
    writer.Bool(always_on_top);
  }
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
  bool new_maximized_horizontally = maximized_horizontally;
  bool new_maximized_vertically = maximized_vertically;
  for (auto& key : ObjectView(d, status)) {
    if (key == "maximized") {
      for (auto& maximized_key : ObjectView(d, status)) {
        if (maximized_key == "horizontally") {
          d.Get(new_maximized_horizontally, status);
        } else if (maximized_key == "vertically") {
          d.Get(new_maximized_vertically, status);
        }
      }
    } else if (key == "output_device_x") {
      d.Get(output_device_x, status);
    } else if (key == "output_device_y") {
      d.Get(output_device_y, status);
    } else if (key == "always_on_top") {
      d.Get(always_on_top, status);
    } else if (key == "width") {
      d.Get(new_size.width, status);
    } else if (key == "height") {
      d.Get(new_size.height, status);
    } else if (key == "camera") {
      for (auto& camera_key : ObjectView(d, status)) {
        if (camera_key == "x") {
          d.Get(camera_x.target, status);
          camera_x.value = camera_x.target;
        } else if (camera_key == "y") {
          d.Get(camera_y.target, status);
          camera_y.value = camera_y.target;
        } else if (camera_key == "zoom") {
          d.Get(zoom.target, status);
          zoom.value = zoom.target;
        }
      }
    }
  }
  if (new_size != size && RequestResize) {
    RequestResize(new_size);
  }
  if (((maximized_horizontally != new_maximized_horizontally) ||
       (maximized_vertically != new_maximized_vertically)) &&
      RequestMaximize) {
    RequestMaximize(new_maximized_horizontally,
                    new_maximized_vertically);  // always true because of the if condition
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

void Window::DropLocation(std::unique_ptr<Location>&& location) {
  // do nothing - location will be deleted by unique_ptr
}

static void UpdateConnectionWidgets(Window& window) {
  for (auto& loc : root_machine->locations) {
    if (loc->object) {
      loc->object->Args([&](Argument& arg) {
        // Check if this argument already has a widget.
        bool has_widget = false;
        for (auto& widget : window.connection_widgets) {
          if (&widget->from != loc.get()) {
            continue;
          }
          if (&widget->arg != &arg) {
            continue;
          }
          has_widget = true;
        }
        if (has_widget) {
          return;
        }
        // Create a new widget.
        LOG << "Creating a ConnectionWidget for argument " << arg.name;
        window.connection_widgets.emplace_back(new gui::ConnectionWidget(*loc, arg));
      });
    }
  }
}

ControlFlow Window::VisitChildren(Visitor& visitor) {
  UpdateConnectionWidgets(*this);
  Vec<Widget*> widgets;
  widgets.reserve(2 + pointers.size() + connection_widgets.size());

  unordered_set<Location*> dragged_locations(pointers.size());
  for (auto* pointer : pointers) {
    if (auto& action = pointer->action) {
      if (auto* drag_action = dynamic_cast<DragLocationAction*>(action.get())) {
        dragged_locations.insert(drag_action->location.get());
      }
    }
  }
  Vec<Widget*> connection_widgets_below;
  connection_widgets_below.reserve(connection_widgets.size());
  for (auto& it : connection_widgets) {
    if (it->manual_position.has_value() || dragged_locations.count(&it->from)) {
      widgets.push_back(it.get());
    } else {
      connection_widgets_below.push_back(it.get());
    }
  }
  for (auto& pointer : pointers) {
    if (auto widget = pointer->GetWidget()) {
      widgets.push_back(widget);
    }
  }
  widgets.push_back(&toolbar);
  for (auto w : connection_widgets_below) {
    widgets.push_back(w);
  }
  widgets.push_back(root_machine);
  return visitor(widgets);
}

}  // namespace automat::gui
