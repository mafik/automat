// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "pointer.hh"

#include <include/effects/SkDashPathEffect.h>
#include <include/pathops/SkPathOps.h>

#include <algorithm>
#include <cassert>

#include "action.hh"
#include "arcline.hh"
#include "automat.hh"
#include "control_flow.hh"
#include "object.hh"
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
  // For small changes we skip the animation to increase responsiveness.
  if (fabs(delta) < 1.0) {
    Vec2 mouse_pre = root_widget.PointerToCanvas().mapPoint(pointer_position);
    root_widget.zoom *= factor;
    Vec2 mouse_post = root_widget.PointerToCanvas().mapPoint(pointer_position);
    Vec2 mouse_delta = mouse_pre - mouse_post;
    root_widget.camera_target += mouse_delta;
    root_widget.camera_pos += mouse_delta;
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
  auto* mw = root_widget.toys.FindOrNull(*root_machine);
  if (!mw) return pointer_position;
  SkMatrix transform_down = TransformDown(*mw);
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

animation::Phase PointerWidget::Tick(time::Timer& timer) {
  struct Highlighted {
    ObjectToy* widget;
    Atom* atom;

    bool operator<(const Highlighted& other) const {
      if (widget == other.widget) {
        return atom < other.atom;
      }
      return widget < other.widget;
    }

    auto operator<=>(const HighlightState& other) const {
      if (widget == other.widget) {
        return atom <=> other.atom;
      }
      return widget <=> other.widget;
    }
  };

  std::vector<Highlighted> highlight_target;
  auto HighlightCheck = [&](Object& obj, ObjectToy& widget, Atom& atom) {
    for (auto& action : pointer.actions) {
      if (action == nullptr) continue;
      if (action->Highlight(obj, atom)) {
        highlight_target.emplace_back(&widget, &atom);
        std::push_heap(highlight_target.begin(), highlight_target.end());
      }
    }
  };
  // Note: this could only iterate over visible locations
  for (auto& loc : root_machine->locations) {
    auto& obj = *loc->object;
    if (!loc->widget || !loc->widget->toy) continue;
    HighlightCheck(obj, *loc->widget->toy, obj);
    obj.Atoms([&](Atom& atom) {
      HighlightCheck(obj, *loc->widget->toy, atom);
      return LoopControl::Continue;
    });
  }

  sort_heap(highlight_target.begin(), highlight_target.end());

  auto target_it = highlight_target.begin();
  auto current_it = highlight_current.begin();
  auto TargetValid = [&]() { return target_it != highlight_target.end(); };
  auto CurrentValid = [&]() { return current_it != highlight_current.end(); };

  std::vector<HighlightState> highlight_next;

  auto phase = animation::Finished;

  auto HighlightTick = [&](ObjectToy* obj, Atom* atom, float current, float target) {
    phase |= animation::ExponentialApproach(target, timer.d, 0.1, current);
    if (current > 0.01f) {
      highlight_next.emplace_back(obj, atom, current);
      phase = animation::Animating;
    }
  };

  if (TargetValid() && CurrentValid()) {
    while (true) {
      if (*target_it < *current_it) {
        // Fade in (new)
        HighlightTick(std::move(target_it->widget), target_it->atom, 0, 1);
        ++target_it;
        if (!TargetValid()) break;
      } else if (*target_it > *current_it) {
        // Fade out
        HighlightTick(std::move(current_it->widget), current_it->atom, current_it->highlight, 0);
        ++current_it;
        if (!CurrentValid()) break;
      } else {
        // Fade in (old)
        HighlightTick(std::move(target_it->widget), target_it->atom, current_it->highlight, 1);
        ++target_it;
        ++current_it;
        if (!TargetValid()) break;
        if (!CurrentValid()) break;
      }
    }
  }

  while (TargetValid()) {
    // Fade in (new)
    HighlightTick(std::move(target_it->widget), target_it->atom, 0, 1);
    ++target_it;
  }

  while (CurrentValid()) {
    // Fade out
    HighlightTick(std::move(current_it->widget), current_it->atom, current_it->highlight, 0);
    ++current_it;
  }

  highlight_current = std::move(highlight_next);

  if (phase != animation::Finished) {
    time_seconds = timer.NowSeconds();
  }

  return phase;
}

SkPath Outset(const SkPath& path, float distance) {
  SkRRect rrect;
  if (path.isRRect(&rrect)) {
    rrect.outset(distance, distance);
    return SkPath::RRect(rrect);
  } else {
    SkPath combined_path;
    bool simplified = Simplify(path, &combined_path);
    ArcLine arcline = ArcLine::MakeFromPath(simplified ? combined_path : path);
    arcline.Outset(distance);
    return arcline.ToPath();
  }
}

void PointerWidget::Draw(SkCanvas& canvas) const {
  for (auto& state : highlight_current) {
    auto shape = state.widget->AtomShape(state.atom);
    SkPath outset_shape = Outset(shape, 2.5_mm * state.highlight);
    outset_shape.setIsVolatile(true);
    canvas.save();
    canvas.concat(TransformBetween(*state.widget, *this));
    static const SkPaint kHighlightPaint = [] {
      SkPaint paint;
      paint.setAntiAlias(true);
      paint.setStyle(SkPaint::kStroke_Style);
      paint.setStrokeWidth(0.0005);
      paint.setColor(0xffa87347);
      return paint;
    }();
    SkPaint dash_paint(kHighlightPaint);
    dash_paint.setAlphaf(state.highlight);
    float intervals[] = {0.0035, 0.0015};
    float period_seconds = 200;
    float phase = std::fmod(time_seconds, period_seconds) / period_seconds;
    dash_paint.setPathEffect(SkDashPathEffect::Make(intervals, phase));
    canvas.drawPath(outset_shape, dash_paint);
    canvas.restore();
  }
  DrawChildren(canvas);
  return;
}

}  // namespace automat::ui
