#include "pointer_impl.h"

#include <algorithm>

#include "keyboard_impl.h"
#include "root.h"
#include "window_impl.h"

namespace automat::gui {

PointerImpl::PointerImpl(WindowImpl& window, Pointer& facade, vec2 position)
    : window(window),
      facade(facade),
      pointer_position(position),
      button_down_position(),
      button_down_time() {
  window.pointers.push_back(this);
  assert(!window.keyboards.empty());
  keyboard = window.keyboards.front();
  keyboard->pointer = this;
}
PointerImpl::~PointerImpl() {
  if (!path.empty()) {
    path.back()->PointerLeave(facade, window.actx);
  }
  if (keyboard) {
    keyboard->pointer = nullptr;
  }
  auto it = std::find(window.pointers.begin(), window.pointers.end(), this);
  if (it != window.pointers.end()) {
    window.pointers.erase(it);
  }
}
void PointerImpl::Move(vec2 position) {
  vec2 old_mouse_pos = pointer_position;
  pointer_position = position;
  if (button_down_time[kMouseMiddle] > time::kZero) {
    vec2 delta = window.WindowToCanvas(position) - window.WindowToCanvas(old_mouse_pos);
    window.camera_x.Shift(-delta.X);
    window.camera_y.Shift(-delta.Y);
    window.inertia = false;
  }
  if (action) {
    action->Update(facade);
  } else {
    Widget* old_hovered_widget = path.empty() ? nullptr : path.back();

    path.clear();
    SkPoint point = SkPoint::Make(pointer_position.X, pointer_position.Y);

    Visitor dfs = [&](Widget& child) {
      SkPoint transformed;
      if (!path.empty()) {
        transformed = path.back()->TransformToChild(&child, window.actx).mapPoint(point);
      } else {
        transformed = point;
      }

      auto shape = child.Shape();
      if (shape.isEmpty() || shape.contains(transformed.fX, transformed.fY)) {
        path.push_back(&child);
        point = transformed;
        child.VisitChildren(dfs);
        return VisitResult::kStop;
      }
      return VisitResult::kContinue;
    };

    dfs(window);

    Widget* hovered_widget = path.empty() ? nullptr : path.back();
    if (old_hovered_widget != hovered_widget) {
      if (old_hovered_widget) {
        old_hovered_widget->PointerLeave(facade, window.actx);
      }
      if (hovered_widget) {
        hovered_widget->PointerOver(facade, window.actx);
      }
    }
  }
}
void PointerImpl::Wheel(float delta) {
  float factor = exp(delta / 4);
  window.zoom.target *= factor;
  // For small changes we skip the animation to increase responsiveness.
  if (fabs(delta) < 1.0) {
    vec2 mouse_pre = window.WindowToCanvas(pointer_position);
    window.zoom.value *= factor;
    vec2 mouse_post = window.WindowToCanvas(pointer_position);
    vec2 mouse_delta = mouse_post - mouse_pre;
    window.camera_x.Shift(-mouse_delta.X);
    window.camera_y.Shift(-mouse_delta.Y);
  }
  window.zoom.target = std::max(kMinZoom, window.zoom.target);
}
void PointerImpl::ButtonDown(PointerButton btn) {
  if (btn == kButtonUnknown || btn >= kButtonCount) return;
  RunOnAutomatThread([=]() {
    button_down_position[btn] = pointer_position;
    button_down_time[btn] = time::now();

    if (action == nullptr && !path.empty()) {
      action = path.back()->ButtonDownAction(facade, btn);
      if (action) {
        action->Begin(facade);
      }
    }
  });
}
void PointerImpl::ButtonUp(PointerButton btn) {
  if (btn == kButtonUnknown || btn >= kButtonCount) return;
  RunOnAutomatThread([=]() {
    if (btn == kMouseLeft) {
      if (action) {
        action->End();
        action.reset();
      }
    }
    if (btn == kMouseMiddle) {
      time::duration down_duration = time::now() - button_down_time[kMouseMiddle];
      vec2 delta = pointer_position - button_down_position[kMouseMiddle];
      float delta_m = Length(delta);
      if ((down_duration < kClickTimeout) && (delta_m < kClickRadius)) {
        vec2 canvas_pos = window.WindowToCanvas(pointer_position);
        window.camera_x.target = canvas_pos.X;
        window.camera_y.target = canvas_pos.Y;
        window.zoom.target = 1;
        window.inertia = false;
      }
    }
    button_down_position[btn] = Vec2(0, 0);
    button_down_time[btn] = time::kZero;
  });
}

vec2 PointerImpl::PositionWithin(Widget& widget) const {
  auto it = std::find(path.begin(), path.end(), &widget);
  auto end = it == path.end() ? path.end() : it + 1;
  Path sub_path(path.begin(), end);
  SkMatrix transform_down = TransformDown(sub_path, window.actx);
  return Vec2(transform_down.mapXY(pointer_position.X, pointer_position.Y));
}

vec2 PointerImpl::PositionWithinRootMachine() const {
  gui::Path root_machine_path;
  root_machine_path.push_back(path.front());
  root_machine_path.push_back(root_machine);
  SkMatrix transform_down = TransformDown(root_machine_path, window.actx);
  return Vec2(transform_down.mapXY(pointer_position.X, pointer_position.Y));
}

Keyboard& PointerImpl::Keyboard() { return keyboard->facade; }

}  // namespace automat::gui
