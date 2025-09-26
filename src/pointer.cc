// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "pointer.hh"

#include <cassert>

#include "action.hh"
#include "automat.hh"
#include "root_widget.hh"
#include "time.hh"
#include "widget.hh"

using namespace std;

namespace automat::ui {

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
      button_down_time(),
      pointer_widget(new PointerWidget(&root_widget, *this)) {
  root_widget.pointers.push_back(this);
  keyboard = &root_widget.keyboard;
  keyboard->pointer = this;
  pointer_widget->local_to_parent = SkM44(root_widget.CanvasToWindow());
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
  p.path.emplace_back(&w);
  Vec2 point = TransformDown(w).mapPoint(p.pointer_position);

  auto shape = w.Shape();
  bool p_inside_w = shape.contains(point.x, point.y);
  bool w_is_unbounded = shape.isEmpty();

  if (p_inside_w || w_is_unbounded) {
    for (auto* child : w.Children()) {
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
  auto old_path = std::move(path);

  path.clear();

  root_widget.ValidateHierarchy();

  FillPath(*this, root_widget);

  if (path.empty()) {
    hover.Reset();
  } else {
    hover = path.back();
  }

  // Try to get references to all widgets in the path - during last & this frame.
  // This allows us to notify them about pointer enter/leave events.
  Vec<Widget*> old_path_alive;
  for (auto& w : old_path) {
    if (w) {
      old_path_alive.push_back(w);
    }
  }
  Vec<Widget*> new_path_alive;
  for (auto& w : path) {
    if (w) {
      new_path_alive.push_back(w);
    }
  }

  for (auto* old_w : old_path_alive) {
    if (std::find(new_path_alive.begin(), new_path_alive.end(), old_w) == new_path_alive.end()) {
      old_w->PointerLeave(*this);
    }
  }
  for (auto* new_w : new_path_alive) {
    if (std::find(old_path_alive.begin(), old_path_alive.end(), new_w) == old_path_alive.end()) {
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

  for (auto& action : actions) {
    if (action) {
      action->Update();
    }
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
  auto& action = actions[static_cast<int>(btn)];

  if (grab) {
    grab->grabber.PointerGrabberButtonDown(*grab, btn);
    return;
  }

  UpdatePath();

  if (action == nullptr && hover) {
    // TODO: process this similarly to keyboard shortcuts
    action = hover->FindAction(*this, btn);
    Widget* curr = hover;
    while (action == nullptr && curr->parent) {
      curr = curr->parent;
      action = curr->FindAction(*this, btn);
    }
    if (action) {
      pointer_widget->ValidateHierarchy();
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

  if (actions[static_cast<int>(btn)]) {
    actions[static_cast<int>(btn)].reset();
    UpdatePath();
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

Pointer::IconOverride::IconOverride(Pointer& pointer, IconType icon) : pointer(pointer) {
  IconType old_icon = pointer.Icon();
  it = pointer.icons.insert(pointer.icons.end(), icon);
  IconType new_icon = pointer.Icon();
  if (old_icon != new_icon) {
    pointer.OnIconChanged(old_icon, new_icon);
  }
}

Pointer::IconOverride::~IconOverride() {
  if (it == pointer.icons.end()) {
    return;
  }
  IconType old_icon = pointer.Icon();
  pointer.icons.erase(it);
  IconType new_icon = pointer.Icon();
  if (old_icon != new_icon) {
    pointer.OnIconChanged(old_icon, new_icon);
  }
}

Vec2 Pointer::PositionWithin(const Widget& widget) const {
  SkMatrix transform_down = TransformDown(widget);
  return Vec2(transform_down.mapPoint(pointer_position));
}
Vec2 Pointer::PositionWithinRootMachine() const {
  SkMatrix transform_down = TransformDown(*root_machine);
  return Vec2(transform_down.mapPoint(pointer_position));
}

Str Pointer::ToStr() const {
  Str ret;
  // TODO: avoid using the `path` variable - maybe start from `hover` and go up the tree
  for (auto& w : path) {
    if (!ret.empty()) {
      ret += " -> ";
    }
    ret += w->Name();
    ret += PositionWithin(*w).ToStrMetric();
  }
  return ret;
}

void Pointer::EndAllActions() {
  for (auto& action : actions) {
    action.reset();
  }
  UpdatePath();
}

void Pointer::ReplaceAction(Action& old_action, std::unique_ptr<Action>&& new_action) {
  for (int i = 0; i < static_cast<int>(PointerButton::Count); ++i) {
    if (actions[i].get() == &old_action) {
      actions[i] = std::move(new_action);
      pointer_widget->ValidateHierarchy();
      break;
    }
  }
}

PointerGrab::~PointerGrab() { grabber.ReleaseGrab(*this); }

void PointerGrab::Release() { pointer.grab.reset(); }

PointerGrab& Pointer::RequestGlobalGrab(PointerGrabber& grabber) {
  grab.reset(new PointerGrab(*this, grabber));
  return *grab;
}

Widget* Pointer::GetWidget() { return pointer_widget.get(); }

void Pointer::Logging::Release() {
  int my_index = -1;
  for (int i = 0; i < pointer.loggings.size(); ++i) {
    if (pointer.loggings[i].get() == this) {
      my_index = i;
      break;
    }
  }
  if (my_index == -1) {
    return;
  }
  auto& window = *pointer.root_widget.window;
  pointer.loggings.EraseIndex(my_index);
  window.RegisterInput();
}
}  // namespace automat::ui
