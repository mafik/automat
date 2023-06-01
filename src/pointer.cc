#include "pointer.h"

#include "pointer_impl.h"
#include "window.h"

namespace automat::gui {

Pointer::Pointer(Window &window, vec2 position)
    : impl(std::make_unique<PointerImpl>(*window.impl, *this, position)) {}
Pointer::~Pointer() {}
void Pointer::Move(vec2 position) { impl->Move(position); }
void Pointer::Wheel(float delta) { impl->Wheel(delta); }
void Pointer::ButtonDown(PointerButton btn) { impl->ButtonDown(btn); }
void Pointer::ButtonUp(PointerButton btn) { impl->ButtonUp(btn); }
Pointer::IconType Pointer::Icon() const { return impl->Icon(); }
void Pointer::PushIcon(IconType icon) { impl->PushIcon(icon); }
void Pointer::PopIcon() { impl->PopIcon(); }
const Path &Pointer::Path() const { return impl->path; }
vec2 Pointer::PositionWithin(Widget &widget) const {
  return impl->PositionWithin(widget);
}
vec2 Pointer::PositionWithinRootMachine() const {
  return impl->PositionWithinRootMachine();
}
Keyboard &Pointer::Keyboard() { return impl->Keyboard(); }

} // namespace automat::gui
