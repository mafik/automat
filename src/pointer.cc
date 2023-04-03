#include "pointer.h"

#include "pointer_impl.h"
#include "window.h"

namespace automaton::gui {

Pointer::Pointer(Window &window, vec2 position)
    : impl(std::make_unique<PointerImpl>(*window.impl, *this, position)) {}
Pointer::~Pointer() {}
void Pointer::Move(vec2 position) { impl->Move(position); }
void Pointer::Wheel(float delta) { impl->Wheel(delta); }
void Pointer::ButtonDown(Button btn) { impl->ButtonDown(btn); }
void Pointer::ButtonUp(Button btn) { impl->ButtonUp(btn); }
Pointer::IconType Pointer::Icon() const { return impl->Icon(); }
void Pointer::PushIcon(IconType icon) { impl->PushIcon(icon); }
void Pointer::PopIcon() { impl->PopIcon(); }
vec2 Pointer::PositionWithin(Widget &widget) const {
  return impl->PositionWithin(widget);
}

} // namespace automaton::gui
