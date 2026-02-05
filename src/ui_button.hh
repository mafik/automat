// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkColor.h>

#include <memory>
#include <optional>

#include "animation.hh"
#include "pointer.hh"
#include "ui_constants.hh"
#include "ui_shape_widget.hh"
#include "units.hh"
#include "widget.hh"

namespace automat::ui {

// Helper for widgets that can be clicked. Takes care of changing the pointer icon and animating
// a `highlight` value. Users of this class should make sure to call the `PointerOver`,
// `PointerLeave`, `Tick` and `FindAction` methods.
struct Clickable {
  const Widget& widget;
  int pointers_over = 0;
  int pointers_pressing = 0;
  float highlight = 0;
  Optional<Pointer::IconOverride> hand_icon = std::nullopt;
  std::function<void(Pointer&)> activate = nullptr;

  Clickable(Widget& widget) : widget(widget) {}

  void PointerOver(Pointer&);
  void PointerLeave(Pointer&);
  animation::Phase Tick(time::Timer&);
  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger);
};

struct Button : Widget {
  std::unique_ptr<Widget> child;
  constexpr static float kPressOffset = 0.2_mm;
  Clickable clickable;

  Button(ui::Widget* parent);
  animation::Phase Tick(time::Timer&) override;
  void PreDraw(SkCanvas&) const override;
  void Draw(SkCanvas&) const override;
  virtual SkRRect RRect() const;
  void PointerOver(Pointer& p) override { clickable.PointerOver(p); }
  void PointerLeave(Pointer& p) override { clickable.PointerLeave(p); }
  std::unique_ptr<Action> FindAction(Pointer& p, ActionTrigger a) override {
    return clickable.FindAction(p, a);
  }
  virtual SkColor ForegroundColor() const { return SK_ColorBLACK; }
  virtual SkColor BackgroundColor() const { return SK_ColorWHITE; }
  virtual float PressRatio() const { return clickable.pointers_pressing ? 1 : 0; }
  void FillChildren(Vec<Widget*>& children) override { children.push_back(child.get()); }
  SkRect ChildBounds() const;
  SkPath Shape() const override;
  virtual void Activate(ui::Pointer&) { WakeAnimation(); }

  virtual void DrawButtonShadow(SkCanvas& canvas, SkColor bg) const;
  virtual void DrawButtonFace(SkCanvas&, SkColor bg, SkColor fg) const;

  Optional<Rect> TextureBounds() const override;

  void UpdateChildTransform();

  // We don't want the children to interact with mouse events.
  bool AllowChildPointerEvents(Widget& child) const override { return false; }
};

struct ColoredButtonArgs {
  SkColor fg = SK_ColorBLACK, bg = SK_ColorWHITE;
  float radius = kMinimalTouchableSize / 2;
  std::function<void(ui::Pointer&)> on_click = nullptr;
};

struct ColoredButton : Button {
  SkColor fg, bg;
  float radius;
  std::function<void(ui::Pointer&)> on_click;

  ColoredButton(ui::Widget* parent, ColoredButtonArgs args = {})
      : Button(parent), fg(args.fg), bg(args.bg), radius(args.radius), on_click(args.on_click) {}

  ColoredButton(ui::Widget* parent, const char* svg_path, ColoredButtonArgs args = {})
      : ColoredButton(parent, args) {
    child = MakeShapeWidget(this, svg_path, SK_ColorWHITE);
    UpdateChildTransform();
  }

  ColoredButton(ui::Widget* parent, SkPath path, ColoredButtonArgs args = {})
      : ColoredButton(parent, args) {
    child = std::make_unique<ShapeWidget>(this, path);
    UpdateChildTransform();
  }

  SkColor ForegroundColor() const override { return fg; }
  SkColor BackgroundColor() const override { return bg; }
  void Activate(ui::Pointer& ptr) override {
    if (on_click) {
      on_click(ptr);
    }
  }

  SkRRect RRect() const override {
    return SkRRect::MakeOval(SkRect::MakeWH(radius * 2, radius * 2));
  };

  bool CenteredAtZero() const override { return true; }
};

struct ToggleButton : Widget {
  std::unique_ptr<Button> on;
  std::unique_ptr<Button> off;

  float filling = 0;
  float time_seconds;  // used for waving animation

  ToggleButton(ui::Widget* parent) : Widget(parent) {}

  void FillChildren(Vec<Widget*>& children) override {
    children.push_back(OnWidget());
    children.push_back(off.get());
  }
  bool AllowChildPointerEvents(Widget& child) const override {
    if (Filled()) {
      return &child == const_cast<ToggleButton*>(this)->OnWidget();
    } else {
      return &child == off.get();
    }
  }

  virtual Button* OnWidget() { return on.get(); }
  animation::Phase Tick(time::Timer&) override;
  void PreDrawChildren(SkCanvas&) const override;
  void DrawChildCached(SkCanvas&, const Widget& child) const override;
  SkRRect RRect() const { return off->RRect(); }
  SkPath Shape() const override { return off->Shape(); }
  Optional<Rect> TextureBounds() const override { return off->TextureBounds(); }

  virtual bool Filled() const { return false; }
};

}  // namespace automat::ui
