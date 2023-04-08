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
  }
  if (action) {
    action->Update(facade);
  } else {
    Widget *old_hovered_widget = hovered_widget;
    hovered_widget = nullptr;
    window.VisitAtPoint(pointer_position,
                        [&](Widget &widget, const SkMatrix &transform) {
                          hovered_widget = &widget;
                          hovered_widget_transform = transform;
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
void PointerImpl::ButtonDown(Button btn) {
  if (btn == kButtonUnknown || btn >= kButtonCount)
    return;
  button_down_position[btn] = pointer_position;
  button_down_time[btn] = time::now();

  if (action == nullptr && hovered_widget) {
    SkPoint local_pointer_position_sk =
        hovered_widget_transform.mapXY(pointer_position.X, pointer_position.Y);
    vec2 local_pointer_position{local_pointer_position_sk.fX,
                                local_pointer_position_sk.fY};
    action =
        hovered_widget->ButtonDownAction(facade, btn, local_pointer_position);
    if (action) {
      action->Begin(facade);
    }
  }
}
void PointerImpl::ButtonUp(Button btn) {
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
    }
  }
  button_down_position[btn] = Vec2(0, 0);
  button_down_time[btn] = time::kZero;
}

vec2 PointerImpl::PositionWithin(Widget &widget) const {
  SkMatrix transform = root_machine->TransformToChild(&widget);
  vec2 canvas_pos = window.WindowToCanvas(pointer_position);
  return Vec2(transform.mapXY(canvas_pos.X, canvas_pos.Y));
}

Keyboard &PointerImpl::Keyboard() {
  return keyboard->facade;
}

} // namespace automaton::gui
