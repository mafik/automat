#include "pointer_impl.hh"

#include <algorithm>

#include "keyboard_impl.hh"
#include "root.hh"
#include "window_impl.hh"

namespace automat::gui {

PointerImpl::PointerImpl(WindowImpl& window, Pointer& facade, Vec2 position)
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
void PointerImpl::Move(Vec2 position) {
  Vec2 old_mouse_pos = pointer_position;
  pointer_position = position;
  if (button_down_time[kMouseMiddle] > time::kZero) {
    Vec2 delta = window.WindowToCanvas(position) - window.WindowToCanvas(old_mouse_pos);
    window.camera_x.Shift(-delta.x);
    window.camera_y.Shift(-delta.y);
    window.inertia = false;
  }
  if (action) {
    action->Update(facade);
  } else {
    Widget* old_hovered_widget = path.empty() ? nullptr : path.back();

    path.clear();
    Vec2 point = pointer_position;

    Visitor dfs = [&](Widget& w) -> ControlFlow {
      Vec2 transformed;
      if (!path.empty()) {
        transformed = path.back()->TransformToChild(w, window.actx).mapPoint(point);
      } else {
        transformed = point;
      }

      auto shape = w.Shape();
      path.push_back(&w);
      std::swap(point, transformed);
      if (shape.isEmpty() || shape.contains(point.x, point.y)) {
        w.VisitChildren(dfs);
        return ControlFlow::Stop;
      } else if (w.ChildrenOutside()) {
        if (w.VisitChildren(dfs) == ControlFlow::Stop) {
          return ControlFlow::Stop;
        }
      }
      std::swap(point, transformed);
      path.pop_back();
      return ControlFlow::Continue;
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
    Vec2 mouse_pre = window.WindowToCanvas(pointer_position);
    window.zoom.value *= factor;
    Vec2 mouse_post = window.WindowToCanvas(pointer_position);
    Vec2 mouse_delta = mouse_post - mouse_pre;
    window.camera_x.Shift(-mouse_delta.x);
    window.camera_y.Shift(-mouse_delta.y);
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
      Vec2 delta = pointer_position - button_down_position[kMouseMiddle];
      float delta_m = Length(delta);
      if ((down_duration < kClickTimeout) && (delta_m < kClickRadius)) {
        Vec2 canvas_pos = window.WindowToCanvas(pointer_position);
        window.camera_x.target = canvas_pos.x;
        window.camera_y.target = canvas_pos.y;
        window.zoom.target = 1;
        window.inertia = false;
      }
    }
    button_down_position[btn] = Vec2(0, 0);
    button_down_time[btn] = time::kZero;
  });
}

Vec2 PointerImpl::PositionWithin(Widget& widget) const {
  auto it = std::find(path.begin(), path.end(), &widget);
  auto end = it == path.end() ? path.end() : it + 1;
  Path sub_path(path.begin(), end);
  SkMatrix transform_down = TransformDown(sub_path, window.actx);
  return Vec2(transform_down.mapXY(pointer_position.x, pointer_position.y));
}

Vec2 PointerImpl::PositionWithinRootMachine() const {
  gui::Path root_machine_path;
  root_machine_path.push_back(path.front());
  root_machine_path.push_back(root_machine);
  SkMatrix transform_down = TransformDown(root_machine_path, window.actx);
  return Vec2(transform_down.mapXY(pointer_position.x, pointer_position.y));
}

Keyboard& PointerImpl::Keyboard() { return keyboard->facade; }

}  // namespace automat::gui
