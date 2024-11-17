// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "pointer.hh"

#include <cassert>

#include "action.hh"
#include "root.hh"
#include "time.hh"
#include "widget.hh"
#include "window.hh"

using namespace maf;

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

Pointer::Pointer(Window& window, Vec2 position)
    : window(window), pointer_position(position), button_down_position(), button_down_time() {
  window.pointers.push_back(this);
  assert(!window.keyboards.empty());
  if (window.keyboards.empty()) {
    keyboard = nullptr;
  } else {
    keyboard = window.keyboards.front();
    keyboard->pointer = this;
  }
}
Pointer::~Pointer() {
  if (hover) {
    hover->PointerLeave(*this, window.display);
  }
  if (keyboard) {
    keyboard->pointer = nullptr;
  }
  auto it = std::find(window.pointers.begin(), window.pointers.end(), this);
  if (it != window.pointers.end()) {
    window.pointers.erase(it);
  }
}
void Pointer::UpdatePath() {
  auto old_path = path;
  path.clear();

  auto& display = window.display;

  path.clear();
  Vec2 point = pointer_position;

  Visitor dfs = [&](Span<std::shared_ptr<Widget>> widgets) -> ControlFlow {
    for (auto w : widgets) {
      Vec2 transformed;
      if (!path.empty()) {
        transformed = w->parent->TransformToChild(*w).mapPoint(point);
      } else {
        transformed = point;
      }

      auto shape = w->Shape();
      path.push_back(w);
      std::swap(point, transformed);
      if (shape.contains(point.x, point.y)) {
        w->PointerVisitChildren(dfs);
        return ControlFlow::Stop;
      } else if (w->TextureBounds(&display) == std::nullopt) {
        if (w->PointerVisitChildren(dfs) == ControlFlow::Stop) {
          return ControlFlow::Stop;
        }
      }
      std::swap(point, transformed);
      path.pop_back();
    }
    return ControlFlow::Continue;
  };

  std::shared_ptr<Widget> window_arr[] = {window.SharedPtr<Widget>()};
  dfs(window_arr);
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
      old_w->PointerLeave(*this, display);
    }
  }
  for (auto& new_w : new_path_shared) {
    if (std::find(old_path_shared.begin(), old_path_shared.end(), new_w) == old_path_shared.end()) {
      new_w->PointerOver(*this, display);
    }
  }
}

void Pointer::Move(Vec2 position) {
  Vec2 old_mouse_pos = pointer_position;
  pointer_position = position;
  if (button_down_time[static_cast<int>(PointerButton::Middle)] > time::kZero) {
    Vec2 delta = window.WindowToCanvas(position) - window.WindowToCanvas(old_mouse_pos);
    window.camera_x.Shift(-delta.x);
    window.camera_y.Shift(-delta.y);
    window.inertia = false;
    window.InvalidateDrawCache();
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
  float factor = exp(delta / 4);
  window.zoom_target *= factor;
  // For small changes we skip the animation to increase responsiveness.
  if (fabs(delta) < 1.0) {
    Vec2 mouse_pre = window.WindowToCanvas(pointer_position);
    window.zoom *= factor;
    Vec2 mouse_post = window.WindowToCanvas(pointer_position);
    Vec2 mouse_delta = mouse_post - mouse_pre;
    window.camera_x.Shift(-mouse_delta.x);
    window.camera_y.Shift(-mouse_delta.y);
  }
  window.zoom_target = std::max(kMinZoom, window.zoom_target);
  window.InvalidateDrawCache();
}

void Pointer::ButtonDown(PointerButton btn) {
  if (btn == PointerButton::Unknown || btn >= PointerButton::Count) return;
  button_down_position[static_cast<int>(btn)] = pointer_position;
  button_down_time[static_cast<int>(btn)] = time::SystemNow();
  UpdatePath();

  if (action == nullptr && hover) {
    // TODO: process this similarly to keyboard shortcuts
    // decide whether it's better to only use the final object for the action vs also its
    // ancestors
    action = hover->FindAction(*this, btn);
    if (action) {
      action->Begin();
      UpdatePath();
    }
  }
}

void Pointer::ButtonUp(PointerButton btn) {
  if (btn == PointerButton::Unknown || btn >= PointerButton::Count) return;

  if (btn == PointerButton::Left) {
    if (action) {
      action->End();
      action.reset();
      UpdatePath();
    }
  }
  if (btn == PointerButton::Middle) {
    time::Duration down_duration =
        time::SystemNow() - button_down_time[static_cast<int>(PointerButton::Middle)];
    Vec2 delta = pointer_position - button_down_position[static_cast<int>(PointerButton::Middle)];
    float delta_m = Length(delta);
    if ((down_duration < kClickTimeout) && (delta_m < kClickRadius)) {
      Vec2 canvas_pos = window.WindowToCanvas(pointer_position);
      window.camera_x.target = canvas_pos.x;
      window.camera_y.target = canvas_pos.y;
      window.zoom_target = 1;
      window.inertia = false;
      window.InvalidateDrawCache();
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
  SkMatrix transform_down = TransformDown(widget, nullptr);
  return Vec2(transform_down.mapXY(pointer_position.x, pointer_position.y));
}
Vec2 Pointer::PositionWithinRootMachine() const {
  SkMatrix transform_down = TransformDown(*root_machine, nullptr);
  return Vec2(transform_down.mapXY(pointer_position.x, pointer_position.y));
}
animation::Display& Pointer::AnimationContext() const { return window.display; }

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

}  // namespace automat::gui
