// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkColor.h>

#include <memory>

#include "animation.hh"
#include "control_flow.hh"
#include "gui_constants.hh"
#include "gui_shape_widget.hh"
#include "pointer.hh"
#include "units.hh"
#include "widget.hh"

namespace automat::gui {

struct Button : Widget {
  constexpr static float kPressOffset = 0.2_mm;

  struct AnimationState {
    int pointers_over = 0;
    float highlight = 0;
  };

  animation::PerDisplay<AnimationState> animation_state_ptr;
  int press_action_count = 0;
  std::unique_ptr<Widget> child;

  Button(std::unique_ptr<Widget>&& child) : child(std::move(child)) {}
  void PointerOver(Pointer&, animation::Display&) override;
  void PointerLeave(Pointer&, animation::Display&) override;
  animation::Phase PreDraw(DrawContext& ctx) const override;
  animation::Phase Draw(DrawContext&) const override;
  float Height() const;
  virtual SkRRect RRect() const;
  SkPath Shape(animation::Display*) const override;
  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) override;
  virtual void Activate(gui::Pointer&) { InvalidateDrawCache(); }
  virtual SkColor ForegroundColor(DrawContext&) const { return SK_ColorBLACK; }  // "#d69d00"_color
  virtual SkColor BackgroundColor() const { return SK_ColorWHITE; }
  virtual float PressRatio() const { return press_action_count ? 1 : 0; }

  void DrawButtonShadow(SkCanvas& canvas, SkColor bg) const;
  virtual void DrawButtonFace(DrawContext&, SkColor bg, SkColor fg) const;

  maf::Optional<Rect> TextureBounds(animation::Display*) const override;

  SkRect ChildBounds() const;

  ControlFlow VisitChildren(Visitor& visitor) override {
    Widget* children[] = {child.get()};
    return visitor(children);
  }

  SkMatrix TransformToChild(const Widget& child, animation::Display*) const override;
  // We don't want the children to interact with mouse events.
  ControlFlow PointerVisitChildren(Visitor& visitor) override { return ControlFlow::Continue; }
};

struct ColoredButtonArgs {
  SkColor fg = SK_ColorBLACK, bg = SK_ColorWHITE;
  float radius = kMinimalTouchableSize / 2;
  std::function<void(gui::Pointer&)> on_click = nullptr;
};

struct ColoredButton : Button {
  SkColor fg, bg;
  float radius;
  std::function<void(gui::Pointer&)> on_click;

  ColoredButton(std::unique_ptr<Widget>&& child, ColoredButtonArgs args = {})
      : Button(std::move(child)),
        fg(args.fg),
        bg(args.bg),
        radius(args.radius),
        on_click(args.on_click) {}

  ColoredButton(const char* svg_path, ColoredButtonArgs args = {})
      : ColoredButton(MakeShapeWidget(svg_path, SK_ColorWHITE), args) {}

  ColoredButton(SkPath path, ColoredButtonArgs args = {})
      : ColoredButton(std::make_unique<ShapeWidget>(path), args) {}

  SkColor ForegroundColor(DrawContext&) const override { return fg; }
  SkColor BackgroundColor() const override { return bg; }
  void Activate(gui::Pointer& ptr) override {
    if (on_click) {
      on_click(ptr);
    }
  }

  SkRRect RRect() const override {
    return SkRRect::MakeOval(SkRect::MakeWH(radius * 2, radius * 2));
  };
};

struct ToggleButton : Widget {
  std::unique_ptr<Button> on;
  std::unique_ptr<Button> off;

  animation::PerDisplay<float> filling_ptr;

  ToggleButton(std::unique_ptr<Button>&& child_on, std::unique_ptr<Button>&& child_off)
      : on(std::move(child_on)), off(std::move(child_off)) {}

  ControlFlow VisitChildren(Visitor& visitor) override {
    Widget* children[] = {OnWidget(), off.get()};
    return visitor(children);
  }
  ControlFlow PointerVisitChildren(Visitor& visitor) override {
    Widget* children[] = {Filled() ? OnWidget() : off.get()};
    return visitor(children);
  }

  virtual Button* OnWidget() const { return on.get(); }
  animation::Phase PreDrawChildren(DrawContext& ctx) const override;
  animation::Phase Draw(DrawContext&) const override;
  animation::Phase DrawChildCachced(DrawContext&, const Widget& child) const override;
  SkRRect RRect() const { return off->RRect(); }
  SkPath Shape(animation::Display* d) const override { return off->Shape(d); }
  maf::Optional<Rect> TextureBounds(animation::Display* d) const override {
    return off->TextureBounds(d);
  }

  virtual bool Filled() const { return false; }
};

}  // namespace automat::gui