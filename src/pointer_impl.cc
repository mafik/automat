#include "pointer_impl.h"

#include "keyboard_impl.h"
#include "root.h"
#include "window_impl.h"

namespace automaton::gui {

PointerImpl::PointerImpl(WindowImpl &window, Pointer &facade, vec2 position)
    : window(window), facade(facade), pointer_position(position),
      button_down_position(), button_down_time() {
  window.pointers.push_back(this);
  assert(!window.keyboards.empty());
  keyboard = window.keyboards.front();
  keyboard->pointer = this;
}
PointerImpl::~PointerImpl() {
  if (hovered_widget) {
    hovered_widget->PointerLeave(facade, window.animation_state);
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
    vec2 delta =
        window.WindowToCanvas(position) - window.WindowToCanvas(old_mouse_pos);
    window.camera_x.Shift(-delta.X);
    window.camera_y.Shift(-delta.Y);
    window.inertia = false;
  }
  if (action) {
    action->Update(facade);
  } else {
    Widget *old_hovered_widget = hovered_widget;
    hovered_widget = nullptr;
    window.VisitAtPoint(pointer_position,
                        [&](Widget &widget, const SkMatrix &transform_down,
                            const SkMatrix &transform_up) {
                          hovered_widget = &widget;
                          return VisitResult::kStop;
                        });
    if (old_hovered_widget != hovered_widget) {
      if (old_hovered_widget) {
        old_hovered_widget->PointerLeave(facade, window.animation_state);
      }
      if (hovered_widget) {
        hovered_widget->PointerOver(facade, window.animation_state);
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
  if (btn == kButtonUnknown || btn >= kButtonCount)
    return;
  button_down_position[btn] = pointer_position;
  button_down_time[btn] = time::now();

  if (action == nullptr && hovered_widget) {
    action = hovered_widget->ButtonDownAction(facade, btn);
    if (action) {
      action->Begin(facade);
    }
  }
}
void PointerImpl::ButtonUp(PointerButton btn) {
  if (btn == kButtonUnknown || btn >= kButtonCount)
    return;
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
}

vec2 PointerImpl::PositionWithin(Widget &widget) const {
  SkMatrix transform_from_root;
  Widget *it = &widget;
  while (true) {
    transform_from_root.preConcat(it->TransformFromParent());
    Widget *next = it->ParentWidget();
    if (next) {
      it = next;
    } else {
      break;
    }
  }
  Widget *root = it;
  vec2 position_within_root;
  if (root == &window) {
    position_within_root = pointer_position;
  } else {
    position_within_root = window.WindowToCanvas(pointer_position);
  }
  return Vec2(transform_from_root.mapXY(position_within_root.X,
                                        position_within_root.Y));
}

Keyboard &PointerImpl::Keyboard() { return keyboard->facade; }

} // namespace automaton::gui
