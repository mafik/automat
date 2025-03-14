// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "pointer.hh"

#include <cassert>

#include "action.hh"
#include "automat.hh"
#include "root_widget.hh"
#include "time.hh"
#include "widget.hh"

using namespace maf;
using namespace std;

namespace automat::gui {

PointerMoveCallback::~PointerMoveCallback() {
  for (Pointer* p : pointers) {
    p->move_callbacks.Erase(this);
  }
  pointers.clear();
}
void PointerMoveCallback::StartWatching(Pointer& p) {
  pointers.push_back(&p);
  p.move_callbacks.push_back(this);
}
void PointerMoveCallback::StopWatching(Pointer& p) {
  pointers.Erase(&p);
  p.move_callbacks.Erase(this);
}

Pointer::Pointer(RootWidget& root_widget, Vec2 position)
    : root_widget(root_widget),
      pointer_position(position),
      button_down_position(),
      button_down_time() {
  root_widget.pointers.push_back(this);
  assert(!root_widget.keyboards.empty());
  if (root_widget.keyboards.empty()) {
    keyboard = nullptr;
  } else {
    keyboard = root_widget.keyboards.front().get();
    keyboard->pointer = this;
  }
}
Pointer::~Pointer() {
  if (hover) {
    hover->PointerLeave(*this);
  }
  if (keyboard) {
    keyboard->pointer = nullptr;
  }
  auto it = std::find(root_widget.pointers.begin(), root_widget.pointers.end(), this);
  if (it != root_widget.pointers.end()) {
    root_widget.pointers.erase(it);
  }
}

static bool FillPath(Pointer& p, Widget& w) {
  p.path.push_back(w.WeakPtr());
  Vec2 point = TransformDown(w).mapPoint(p.pointer_position);
  auto shape = w.Shape();
  bool p_inside_w = shape.contains(point.x, point.y);
  bool w_is_unbounded = w.pack_frame_texture_bounds == std::nullopt;

  if (p_inside_w || w_is_unbounded) {
    for (auto& child : w.Children()) {
      if (w.AllowChildPointerEvents(*child)) {
        if (FillPath(p, *child)) {
          return true;
        }
      }
    }
  }
  // This condition happens at most once per search. All of the parent stack frames are
  // short-circuited by `return true`.
  if (p_inside_w) {
    return true;
  }

  p.path.pop_back();
  return false;
}

void Pointer::UpdatePath() {
  auto old_path = path;

  path.clear();

  FillPath(*this, root_widget);

  hover = path.empty() ? nullptr : path.back().lock();

  // Try to get references to all widgets in the path - during last & this frame.
  // This allows us to notify them about pointer enter/leave events.
  Vec<shared_ptr<Widget>> old_path_shared;
  for (auto& w : old_path) {
    if (auto s = w.lock()) {
      old_path_shared.push_back(s);
    }
  }
  Vec<shared_ptr<Widget>> new_path_shared;
  for (auto& w : path) {
    if (auto s = w.lock()) {
      new_path_shared.push_back(s);
    }
  }

  for (auto& old_w : old_path_shared) {
    if (std::find(new_path_shared.begin(), new_path_shared.end(), old_w) == new_path_shared.end()) {
      old_w->PointerLeave(*this);
    }
  }
  for (auto& new_w : new_path_shared) {
    if (std::find(old_path_shared.begin(), old_path_shared.end(), new_w) == old_path_shared.end()) {
      new_w->PointerOver(*this);
    }
  }
}

void Pointer::Move(Vec2 position) {
  Vec2 old_mouse_pos = pointer_position;
  pointer_position = position;
  if (grab) {
    grab->grabber.PointerGrabberMove(*grab, position);
    return;
  }

  auto px2canvas = TransformDown(*root_machine);

  if (button_down_time[static_cast<int>(PointerButton::Middle)] > time::kZero) {
    Vec2 delta = px2canvas.mapPoint(position) - px2canvas.mapPoint(old_mouse_pos);
    root_widget.camera_target -= delta;
    root_widget.camera_pos -= delta;
    root_widget.inertia = false;
    root_widget.WakeAnimation();
  }
  if (action) {
    action->Update();
  }
  UpdatePath();
  for (auto* cb : move_callbacks) {
    cb->PointerMove(*this, position);
  }
}
void Pointer::Wheel(float delta) {
  if (grab) {
    grab->grabber.PointerGrabberWheel(*grab, delta);
    return;
  }

  float factor = exp(delta / 4);
  root_widget.zoom_target *= factor;
  auto position_metric = TransformDown(root_widget).mapPoint(pointer_position);
  // For small changes we skip the animation to increase responsiveness.
  if (fabs(delta) < 1.0) {
    Vec2 mouse_pre = root_widget.WindowToCanvas().mapPoint(pointer_position);
    root_widget.zoom *= factor;
    Vec2 mouse_post = root_widget.WindowToCanvas().mapPoint(pointer_position);
    Vec2 mouse_delta = mouse_post - mouse_pre;
    root_widget.camera_target -= mouse_delta;
    root_widget.camera_pos -= mouse_delta;
  }
  root_widget.zoom_target = std::max(kMinZoom, root_widget.zoom_target);
  root_widget.WakeAnimation();
}

void Pointer::ButtonDown(PointerButton btn) {
  if (btn == PointerButton::Unknown || btn >= PointerButton::Count) return;
  button_down_position[static_cast<int>(btn)] = pointer_position;
  button_down_time[static_cast<int>(btn)] = time::SystemNow();

  if (grab) {
    grab->grabber.PointerGrabberButtonDown(*grab, btn);
    return;
  }

  UpdatePath();

  if (action == nullptr && hover) {
    // TODO: process this similarly to keyboard shortcuts
    action = hover->FindAction(*this, btn);
    std::shared_ptr<Widget>* curr = &hover;
    while (action == nullptr && (*curr)->parent) {
      curr = &(*curr)->parent;
      action = (*curr)->FindAction(*this, btn);
    }
    if (action) {
      action->Begin();
      UpdatePath();
    }
  }
}

void Pointer::ButtonUp(PointerButton btn) {
  if (btn == PointerButton::Unknown || btn >= PointerButton::Count) return;

  if (grab) {
    grab->grabber.PointerGrabberButtonUp(*grab, btn);
    return;
  }

  if (btn == PointerButton::Left) {
    EndAction();
  }
  if (btn == PointerButton::Middle) {
    time::Duration down_duration =
        time::SystemNow() - button_down_time[static_cast<int>(PointerButton::Middle)];
    Vec2 delta = pointer_position - button_down_position[static_cast<int>(PointerButton::Middle)];
    float delta_m = Length(delta);
    if ((down_duration < kClickTimeout) && (delta_m < kClickRadius)) {
      Vec2 canvas_pos = TransformDown(*root_machine).mapPoint(pointer_position);
      root_widget.camera_target = canvas_pos;
      root_widget.zoom_target = 1;
      root_widget.inertia = false;
      root_widget.WakeAnimation();
    }
  }
  button_down_position[static_cast<int>(btn)] = Vec2(0, 0);
  button_down_time[static_cast<int>(btn)] = time::kZero;
}
Pointer::IconType Pointer::Icon() const {
  if (icons.empty()) {
    return Pointer::kIconArrow;
  }
  return icons.back();
}
void Pointer::PushIcon(IconType icon) { icons.push_back(icon); }
void Pointer::PopIcon() { icons.pop_back(); }
Vec2 Pointer::PositionWithin(const Widget& widget) const {
  SkMatrix transform_down = TransformDown(widget);
  return Vec2(transform_down.mapXY(pointer_position.x, pointer_position.y));
}
Vec2 Pointer::PositionWithinRootMachine() const {
  SkMatrix transform_down = TransformDown(*root_machine);
  return Vec2(transform_down.mapXY(pointer_position.x, pointer_position.y));
}

Str Pointer::ToStr() const {
  Str ret;
  // TODO: avoid using the `path` variable - maybe start from `hover` and go up the tree
  for (auto& w : path) {
    if (!ret.empty()) {
      ret += " -> ";
    }
    ret += w.lock()->Name();
    ret += PositionWithin(*w.lock()).ToStrMetric();
  }
  return ret;
}

void Pointer::EndAction() {
  if (action) {
    action->End();
    action.reset();
    UpdatePath();
  }
}

void PointerGrab::Release() {
  grabber.ReleaseGrab(*this);
  pointer.grab.reset();  // PointerGrab deletes itself here!
}

PointerGrab& Pointer::RequestGlobalGrab(PointerGrabber& grabber) {
  if (grab) {
    grab->Release();
  }
  grab.reset(new PointerGrab(*this, grabber));
  return *grab;
}

}  // namespace automat::gui
