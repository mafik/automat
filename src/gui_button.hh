// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <include/core/SkColor.h>

#include <memory>

#include "animation.hh"
#include "gui_constants.hh"
#include "gui_shape_widget.hh"
#include "pointer.hh"
#include "units.hh"
#include "widget.hh"

namespace automat::gui {

// Base class for things that can be clicked. Takes care of changing the pointer icon and animating
// a `highlight` value.
struct Clickable : Widget {
  Ptr<Widget> child;

  int pointers_over = 0;
  int pointers_pressing = 0;
  float highlight = 0;

  Clickable(Ptr<Widget> child) : child(child) {}

  void FillChildren(maf::Vec<Ptr<Widget>>& children) override { children.push_back(child); }
  virtual SkRRect RRect() const;
  void Draw(SkCanvas&) const override;
  SkPath Shape() const override;
  SkRect ChildBounds() const;
  void PointerOver(Pointer&) override;
  void PointerLeave(Pointer&) override;
  animation::Phase Tick(time::Timer&) override;
  virtual void Activate(gui::Pointer&) { WakeAnimation(); }
  std::unique_ptr<Action> FindAction(Pointer&, ActionTrigger) override;
};

struct Button : Clickable {
  constexpr static float kPressOffset = 0.2_mm;

  Button(Ptr<Widget> child);
  animation::Phase Tick(time::Timer&) override;
  void PreDraw(SkCanvas&) const override;
  void Draw(SkCanvas&) const override;
  SkRRect RRect() const override;
  virtual SkColor ForegroundColor() const { return SK_ColorBLACK; }
  virtual SkColor BackgroundColor() const { return SK_ColorWHITE; }
  virtual float PressRatio() const { return pointers_pressing ? 1 : 0; }

  virtual void DrawButtonShadow(SkCanvas& canvas, SkColor bg) const;
  virtual void DrawButtonFace(SkCanvas&, SkColor bg, SkColor fg) const;

  maf::Optional<Rect> TextureBounds() const override;

  void UpdateChildTransform();

  // We don't want the children to interact with mouse events.
  bool AllowChildPointerEvents(Widget& child) const override { return false; }
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

  ColoredButton(Ptr<Widget> child, ColoredButtonArgs args = {})
      : Button(child), fg(args.fg), bg(args.bg), radius(args.radius), on_click(args.on_click) {
    UpdateChildTransform();
  }

  ColoredButton(const char* svg_path, ColoredButtonArgs args = {})
      : ColoredButton(MakeShapeWidget(svg_path, SK_ColorWHITE), args) {
    UpdateChildTransform();
  }

  ColoredButton(SkPath path, ColoredButtonArgs args = {})
      : ColoredButton(MakePtr<ShapeWidget>(path), args) {
    UpdateChildTransform();
  }

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

  bool CenteredAtZero() const override { return true; }
};

struct ToggleButton : Widget {
  Ptr<Button> on;
  Ptr<Button> off;

  float filling = 0;
  float time_seconds;  // used for waving animation

  ToggleButton(Ptr<Button> on, Ptr<Button> off) : on(on), off(off) {}

  void FillChildren(maf::Vec<Ptr<Widget>>& children) override {
    children.push_back(OnWidget());
    children.push_back(off);
  }
  bool AllowChildPointerEvents(Widget& child) const override {
    if (Filled()) {
      return &child == const_cast<ToggleButton*>(this)->OnWidget().get();
    } else {
      return &child == off.get();
    }
  }

  virtual Ptr<Button>& OnWidget() { return on; }
  animation::Phase Tick(time::Timer&) override;
  void PreDrawChildren(SkCanvas&) const override;
  void DrawChildCachced(SkCanvas&, const Widget& child) const override;
  SkRRect RRect() const { return off->RRect(); }
  SkPath Shape() const override { return off->Shape(); }
  maf::Optional<Rect> TextureBounds() const override { return off->TextureBounds(); }

  virtual bool Filled() const { return false; }
};

}  // namespace automat::gui