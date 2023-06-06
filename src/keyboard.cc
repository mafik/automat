#include "keyboard.h"

#include "keyboard_impl.h"
#include "window.h"
#include "window_impl.h"

namespace automat::gui {

Caret::Caret(CaretImpl& impl) : impl(impl) {}

void Caret::PlaceIBeam(vec2 position) { impl.PlaceIBeam(position); }

CaretOwner::~CaretOwner() {
  for (auto caret : carets) {
    caret->owner = nullptr;
  }
}

Caret& CaretOwner::RequestCaret(Keyboard& keyboard, const Path& widget_path, vec2 position) {
  auto& kb = *keyboard.impl;
  std::set<std::unique_ptr<CaretImpl>>::iterator it;
  if (kb.carets.empty()) {
    it = kb.carets.emplace(std::make_unique<CaretImpl>(kb)).first;
  } else {
    it = kb.carets.begin();
  }
  CaretImpl& caret = **it;
  if (caret.owner) {
    caret.owner->ReleaseCaret(caret.facade);
    caret.owner->carets.erase(
        std::find(caret.owner->carets.begin(), caret.owner->carets.end(), &caret));
  }
  caret.owner = this;
  caret.widget_path = widget_path;
  caret.PlaceIBeam(position);
  carets.emplace_back(&caret);
  return caret.facade;
}

void CaretOwner::KeyDown(Caret& caret, Key) {}
void CaretOwner::KeyUp(Caret& caret, Key) {}

Keyboard::Keyboard(Window& window) : impl(std::make_unique<KeyboardImpl>(*window.impl, *this)) {}

Keyboard::~Keyboard() {}

void Keyboard::Draw(SkCanvas& canvas, animation::State& animation_state) const {
  impl->Draw(canvas, animation_state);
}

void Keyboard::KeyDown(Key key) { impl->KeyDown(key); }
void Keyboard::KeyUp(Key key) { impl->KeyUp(key); }

}  // namespace automat::gui
