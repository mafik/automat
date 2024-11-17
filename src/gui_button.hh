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

  mutable AnimationState animation_state;
  int press_action_count = 0;
  std::shared_ptr<Widget> child;

  Button(std::shared_ptr<Widget> child) : child(child) {}
  void PointerOver(Pointer&, animation::Display&) override;
  void PointerLeave(Pointer&, animation::Display&) override;
  animation::Phase Update(animation::Display&) override;
  animation::Phase PreDraw(DrawContext& ctx) const override;
  animation::Phase Draw(DrawContext&) const override;
  float Height() const;
  virtual SkRRect RRect() const;
  SkPath Shape() const override;
  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) override;
  virtual void Activate(gui::Pointer&) { InvalidateDrawCache(); }
  virtual SkColor ForegroundColor() const { return SK_ColorBLACK; }  // "#d69d00"_color
  virtual SkColor BackgroundColor() const { return SK_ColorWHITE; }
  virtual float PressRatio() const { return press_action_count ? 1 : 0; }

  void DrawButtonShadow(SkCanvas& canvas, SkColor bg) const;
  virtual void DrawButtonFace(DrawContext&, SkColor bg, SkColor fg) const;

  maf::Optional<Rect> TextureBounds(animation::Display*) const override;

  SkRect ChildBounds() const;

  ControlFlow VisitChildren(Visitor& visitor) override {
    return visitor(maf::SpanOfArr(&child, 1));
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

  ColoredButton(std::shared_ptr<Widget> child, ColoredButtonArgs args = {})
      : Button(child), fg(args.fg), bg(args.bg), radius(args.radius), on_click(args.on_click) {}

  ColoredButton(const char* svg_path, ColoredButtonArgs args = {})
      : ColoredButton(MakeShapeWidget(svg_path, SK_ColorWHITE), args) {}

  ColoredButton(SkPath path, ColoredButtonArgs args = {})
      : ColoredButton(std::make_shared<ShapeWidget>(path), args) {}

  SkColor ForegroundColor() const override { return fg; }
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
  std::shared_ptr<Button> on;
  std::shared_ptr<Button> off;

  mutable float filling;

  ToggleButton(std::shared_ptr<Button> on, std::shared_ptr<Button> off) : on(on), off(off) {}

  ControlFlow VisitChildren(Visitor& visitor) override {
    std::shared_ptr<Widget> children[] = {OnWidget(), off};
    return visitor(children);
  }
  ControlFlow PointerVisitChildren(Visitor& visitor) override {
    std::shared_ptr<Widget> children[] = {Filled() ? OnWidget() : off};
    return visitor(children);
  }

  virtual std::shared_ptr<Button>& OnWidget() { return on; }
  animation::Phase PreDrawChildren(DrawContext& ctx) const override;
  animation::Phase Draw(DrawContext&) const override;
  animation::Phase DrawChildCachced(DrawContext&, const Widget& child) const override;
  SkRRect RRect() const { return off->RRect(); }
  SkPath Shape() const override { return off->Shape(); }
  maf::Optional<Rect> TextureBounds(animation::Display* d) const override {
    return off->TextureBounds(d);
  }

  virtual bool Filled() const { return false; }
};

}  // namespace automat::gui