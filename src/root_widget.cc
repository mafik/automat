// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "root_widget.hh"

#include <include/core/SkPath.h>

#include <memory>

#include "animation.hh"
#include "automat.hh"
#include "drag_action.hh"
#include "embedded.hh"
#include "font.hh"
#include "gui_connection_widget.hh"
#include "math.hh"
#include "pointer.hh"
#include "prototypes.hh"
#include "touchpad.hh"

using namespace maf;
using namespace std;

namespace automat::gui {

std::vector<RootWidget*> root_widgets;
std::shared_ptr<RootWidget> root_widget;

RootWidget::RootWidget() { root_widgets.push_back(this); }
RootWidget::~RootWidget() {
  auto it = std::find(root_widgets.begin(), root_widgets.end(), this);
  if (it != root_widgets.end()) {
    root_widgets.erase(it);
  }
}

void RootWidget::InitToolbar() {
  toolbar = std::make_shared<Toolbar>();
  toolbar->parent = SharedPtr();
  for (auto& [_, proto] : prototypes->name_index) {
    toolbar->AddObjectPrototype(proto);
  }
}

static SkColor background_color = SkColorSetRGB(0x80, 0x80, 0x80);
constexpr float kTrashRadius = 3_cm;

animation::Phase RootWidget::Tick(time::Timer& timer) {
  auto phase = animation::Finished;

  // Record camera movement timeline. This is used to create inertia effect.
  camera_timeline.emplace_back(Vec3(camera_pos, zoom));
  timeline.emplace_back(timer.now);
  while (timeline.front() < timer.now - time::Duration(0.2)) {
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
    camera_target += total_pan / zoom;
    camera_pos += total_pan / zoom;
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
      Vec2 shift = Vec2(dx, dy) / dt * timer.d * 0.8;
      camera_pos += shift;
      camera_target += shift;
      float z = pow(dz, timer.d / dt * 0.8);
      zoom_target *= z;
      zoom *= z;
      float lz = logf(z);
      float dpx = sqrtf(dx * dx + dy * dy + lz * lz) * PxPerMeter();
      if (dpx < 1) {
        inertia = false;
      }
    }
  }

  if (inertia) {
    phase = animation::Animating;
  }

  float rx = camera_target.x - camera_pos.x;
  float ry = camera_target.y - camera_pos.y;
  float rz = fabsf(zoom - zoom_target);
  float r = Length(Vec2(rx, ry));
  float rpx = PxPerMeter() * r;
  bool stabilize_mouse = rpx < 1;

  if (stabilize_mouse) {
    if (pointers.size() > 0) {
      Pointer* first_pointer = *pointers.begin();
      Vec2 mouse_position = TransformDown(*this).mapPoint(first_pointer->pointer_position);
      Vec2 focus_pre = WindowToCanvas().mapPoint(mouse_position);
      phase |= animation::ExponentialApproach(zoom_target, timer.d, 1.0 / 15, zoom);
      Vec2 focus_post = WindowToCanvas().mapPoint(mouse_position);
      Vec2 focus_delta = focus_pre - focus_post;
      camera_pos += focus_delta;
      camera_target += focus_delta;
    }
  } else {  // stabilize camera target
    Vec2 focus_pre = camera_target;

    Vec2 target_screen = CanvasToWindow().mapPoint(focus_pre);
    phase |= animation::ExponentialApproach(zoom_target, timer.d, 1.0 / 15, zoom);
    Vec2 focus_post = WindowToCanvas().mapPoint(target_screen);
    Vec2 focus_delta = focus_post - focus_pre;
    camera_pos -= focus_delta;
  }

  phase |= animation::ExponentialApproach(camera_target.x, timer.d, 0.1, camera_pos.x);
  phase |= animation::ExponentialApproach(camera_target.y, timer.d, 0.1, camera_pos.y);

  if (move_velocity.x != 0) {
    float shift_x = move_velocity.x * timer.d;
    camera_pos.x += shift_x;
    camera_target.x += shift_x;
    inertia = false;
    phase = animation::Animating;
  }
  if (move_velocity.y != 0) {
    float shift_y = move_velocity.y * timer.d;
    camera_pos.y += shift_y;
    camera_target.y += shift_y;
    inertia = false;
    phase = animation::Animating;
  }

  SkRect work_area = SkRect::MakeXYWH(-0.5, -0.5, 1, 1);

  // Make sure that work area doesn't leave the root widget bounds (so the user
  // doesn't get lost)
  {
    // Leave 1mm of margin so that the user can still see the edge of the
    // work area
    Rect window_bounds =
        WindowToCanvas().mapRect(SkRect::MakeLTRB(1_mm, 1_mm, size.x - 1_mm, size.y - 1_mm));
    if (work_area.left() > window_bounds.right) {
      float shift_x = work_area.left() - window_bounds.right;
      camera_pos.x += shift_x;
      camera_target.x += shift_x;
    }
    if (work_area.right() < window_bounds.left) {
      float shift_x = work_area.right() - window_bounds.left;
      camera_pos.x += shift_x;
      camera_target.x += shift_x;
    }
    // The y axis is flipped so `work_area.bottom()` is actually its top
    if (work_area.bottom() < window_bounds.bottom) {
      float shift_y = work_area.bottom() - window_bounds.bottom;
      camera_pos.y += shift_y;
      camera_target.y += shift_y;
    }
    if (work_area.top() > window_bounds.top) {
      float shift_y = work_area.top() - window_bounds.top;
      camera_pos.y += shift_y;
      camera_target.y += shift_y;
    }
  }

  {  // Animate trash area
    trash_radius_target = drag_action_count ? kTrashRadius : 0;
    phase |= animation::ExponentialApproach(trash_radius_target, timer.d, 0.1, trash_radius);
  }

  if (phase == animation::Animating) {
    for (auto& each_window : root_widgets) {
      for (auto& each_pointer : each_window->pointers) {
        each_pointer->UpdatePath();
      }
    }
  }

  auto canvas_to_window = SkM44(CanvasToWindow());

  root_machine->local_to_parent = canvas_to_window;
  for (auto& each_keyboard : keyboards) {
    each_keyboard->local_to_parent = canvas_to_window;
  }
  for (auto& pointer : pointers) {
    if (auto& action = pointer->action) {
      if (auto* widget = action->Widget()) {
        widget->local_to_parent = canvas_to_window;
      }
    }
  }
  for (auto& each_connection_widget : connection_widgets) {
    each_connection_widget->local_to_parent = canvas_to_window;
  }

  return phase;
}

void RootWidget::Draw(SkCanvas& canvas) const {
  canvas.clear(background_color);

  DrawChildren(canvas);
  canvas.concat(CanvasToWindow());

  // Draw target root_widget size when zooming in with middle mouse button
  float rz = fabsf(zoom - zoom_target);
  if (zoom_target == 1 && rz > 0.001) {
    SkPaint target_paint(SkColor4f(0, 0.3, 0.8, rz));
    target_paint.setStyle(SkPaint::kStroke_Style);
    target_paint.setStrokeWidth(0.001);  // 1mm
    float target_width = size.width;
    float target_height = size.height;
    SkRect target_rect =
        SkRect::MakeXYWH(camera_target.x - target_width / 2, camera_target.y - target_height / 2,
                         target_width, target_height);
    canvas.drawRect(target_rect, target_paint);
  }
}

struct MoveCameraAction : Action {
  RootWidget& root;
  Vec2 delta;
  MoveCameraAction(Pointer& pointer, RootWidget& root, Vec2 delta)
      : Action(pointer), root(root), delta(delta) {}
  ~MoveCameraAction() {
    root.move_velocity -= delta;
    root.WakeAnimation();
  }
  void Begin() override {
    root.move_velocity += delta;
    root.WakeAnimation();
  }
  void Update() override {}
  void End() override {}
};

std::unique_ptr<Action> RootWidget::FindAction(Pointer& p, ActionTrigger trigger) {
  if (trigger == AnsiKey::W) {
    return std::make_unique<MoveCameraAction>(p, *this, Vec2(0, 0.1));
  } else if (trigger == AnsiKey::S) {
    return std::make_unique<MoveCameraAction>(p, *this, Vec2(0, -0.1));
  } else if (trigger == AnsiKey::A) {
    return std::make_unique<MoveCameraAction>(p, *this, Vec2(-0.1, 0));
  } else if (trigger == AnsiKey::D) {
    return std::make_unique<MoveCameraAction>(p, *this, Vec2(0.1, 0));
  }
  return nullptr;
}

void RootWidget::Zoom(float delta) {
  if (pointers.size() > 0) {
    Pointer* first_pointer = *pointers.begin();
    Vec2 mouse_position = TransformDown(*this).mapPoint(first_pointer->pointer_position);
    Vec2 focus_pre = WindowToCanvas().mapPoint(mouse_position);
    zoom_target *= delta;
    zoom *= delta;
    Vec2 focus_post = WindowToCanvas().mapPoint(mouse_position);
    Vec2 focus_delta = focus_post - focus_pre;
    camera_pos -= focus_delta;
    camera_target -= focus_delta;
  } else {
    zoom_target *= delta;
    zoom *= delta;
  }
}

void RootWidget::SerializeState(Serializer& writer) const {
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
  writer.Double(camera_pos.x);
  writer.String("y");
  writer.Double(camera_pos.y);
  writer.String("zoom");
  writer.Double(zoom);
  writer.EndObject();
  writer.EndObject();
}

void RootWidget::DeserializeState(Deserializer& d, Status& status) {
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
          d.Get(camera_target.x, status);
          camera_pos.x = camera_target.x;
        } else if (camera_key == "y") {
          d.Get(camera_target.y, status);
          camera_pos.y = camera_target.y;
        } else if (camera_key == "zoom") {
          d.Get(zoom_target, status);
          zoom = zoom_target;
        }
      }
    }
  }
  if (new_size != size) {
    if (window) {
      window->RequestResize(new_size);
    } else {
      Resized(new_size);
    }
  }
  if ((maximized_horizontally != new_maximized_horizontally) ||
      (maximized_vertically != new_maximized_vertically)) {
    if (window) {
      window->RequestMaximize(new_maximized_horizontally,
                              new_maximized_vertically);  // always true because of the if condition
    } else {
      Maximized(new_maximized_horizontally, new_maximized_vertically);
    }
  }
}

SkPath RootWidget::TrashShape() const {
  SkPath trash_area_path = SkPath::Circle(size.width, size.height, trash_radius);
  trash_area_path.transform(this->WindowToCanvas());
  return trash_area_path;
}

void RootWidget::SnapPosition(Vec2& position, float& scale, Location& location, Vec2* fixed_point) {
  Rect object_bounds = location.WidgetForObject()->Shape().getBounds();
  Rect machine_bounds = root_machine->Shape().getBounds();
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

  Vec2 window_pos = (position - camera_pos) * zoom + size / 2;
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

void RootWidget::DropLocation(std::shared_ptr<Location>&& location) {
  // do nothing - location will be deleted by unique_ptr
  audio::Play(embedded::assets_SFX_trash_wav);
}

static void UpdateConnectionWidgets(RootWidget& root_widget) {
  for (auto& loc : root_machine->locations) {
    if (loc->object) {
      loc->object->Args([&](Argument& arg) {
        // Check if this argument already has a widget.
        bool has_widget = false;
        for (auto& widget : root_widget.connection_widgets) {
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
        root_widget.connection_widgets.emplace_back(new gui::ConnectionWidget(*loc, arg));
        root_widget.connection_widgets.back()->parent = root_widget.SharedPtr();
      });
    }
  }

  for (auto& widget : root_widget.connection_widgets) {
    widget->local_to_parent = SkM44(root_widget.CanvasToWindow());
  }
}

void RootWidget::FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) {
  UpdateConnectionWidgets(*this);
  children.reserve(2 + keyboards.size() + pointers.size() + connection_widgets.size());

  for (auto& keyboard : keyboards) {
    children.push_back(keyboard);
  }

  unordered_set<Location*> dragged_locations(pointers.size());
  for (auto* pointer : pointers) {
    if (auto& action = pointer->action) {
      if (auto* drag_action = dynamic_cast<DragLocationAction*>(action.get())) {
        dragged_locations.insert(drag_action->location.get());
      }
    }
  }
  Vec<shared_ptr<Widget>> connection_widgets_below;
  connection_widgets_below.reserve(connection_widgets.size());
  for (auto& it : connection_widgets) {
    if (it->manual_position.has_value() || dragged_locations.count(&it->from)) {
      children.push_back(it);
    } else {
      connection_widgets_below.push_back(it);
    }
  }
  for (auto& pointer : pointers) {
    if (auto widget = pointer->GetWidget()) {
      children.push_back(widget->SharedPtr<Widget>());
    }
  }
  children.push_back(toolbar);
  for (auto w : connection_widgets_below) {
    children.push_back(w);
  }
  children.push_back(root_machine);
}

static void UpdateLocalToParent(RootWidget& root_widget) {
  float px_per_meter = root_widget.display_pixels_per_meter;
  root_widget.local_to_parent = SkM44::Scale(px_per_meter, -px_per_meter);
  root_widget.local_to_parent.preTranslate(0, -root_widget.size.height);
}

void RootWidget::DisplayPixelDensity(float pixels_per_meter) {
  display_pixels_per_meter = pixels_per_meter;
  UpdateLocalToParent(*this);
}

void RootWidget::Resized(Vec2 size) {
  this->size = size;
  UpdateLocalToParent(*this);
  toolbar->local_to_parent = SkM44(SkMatrix::Translate(size.x / 2, 0));
}

}  // namespace automat::gui