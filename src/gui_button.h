#pragma once

#include <modules/skottie/include/Skottie.h>

#include "animation.h"
#include "product_ptr.h"
#include "widget.h"

namespace automaton::gui {

struct Button : Widget {
  Widget *parent_widget;
  sk_sp<skottie::Animation> picture;
  mutable product_ptr<animation::Approach> inset_shadow_press;
  mutable product_ptr<animation::Approach> inset_shadow_inset;
  int press_action_count = 0;

  Button(Widget *parent_widget, sk_sp<skottie::Animation> picture);
  Widget *ParentWidget() override;
  void PointerOver(Pointer &, animation::State &) override;
  void PointerLeave(Pointer &, animation::State &) override;
  void Draw(SkCanvas &, animation::State &animation_state) const override;
  SkPath Shape() const override;
  std::unique_ptr<Action> ButtonDownAction(Pointer &, PointerButton,
                                           vec2 contact_point) override;
  virtual void Activate() = 0;
};

} // namespace automaton::gui