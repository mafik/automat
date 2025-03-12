// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_window.hh"

#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>

#include "drawing.hh"
#include "gui_button.hh"
#include "gui_shape_widget.hh"
#include "svg.hh"

namespace automat::library {

Window::Window() {}

std::string_view Window::Name() const { return "Window"; }

std::shared_ptr<Object> Window::Clone() const { return std::make_shared<Window>(*this); }

const SkMatrix kCenterPickIcon = SkMatrix::Translate(-1_mm, 0);

constexpr static float kBorderWidth = 0.5_mm;    // width of the border
constexpr static float kContentMargin = 0.5_mm;  // margin between the border and the content
constexpr static float kTitleHeight = 8_mm;      // height of the title bar
constexpr static float kTitleButtonSize = kTitleHeight - 2 * kContentMargin;

struct PickButton : gui::Button {
  PickButton() : gui::Button(gui::MakeShapeWidget(kPickSVG, "#000000"_color, &kCenterPickIcon)) {}
  SkRRect RRect() const override {
    return RRect::MakeSimple(
               Rect::MakeAtZero<::LeftX, ::BottomY>({kTitleButtonSize, kTitleButtonSize}), 1_mm)
        .sk;
  }
  SkColor BackgroundColor() const override { return "#d0d0d0"_color; }
};

struct WindowWidget : gui::Widget {
  std::weak_ptr<Window> window;

  constexpr static float kWidth = 5_cm;
  constexpr static float kCornerRadius = 1_mm;
  constexpr static float kDefaultHeight = 5_cm;

  float height = kDefaultHeight;

  std::shared_ptr<gui::Button> pick_button;

  WindowWidget(std::weak_ptr<Window>&& window) : window(std::move(window)) {
    pick_button = std::make_shared<PickButton>();
    auto content_bounds = CoarseBounds().Outset(-kBorderWidth - kContentMargin);
    auto title_bounds = Rect(content_bounds.rect.left, content_bounds.rect.top - kTitleHeight,
                             content_bounds.rect.right, content_bounds.rect.top);

    auto pos = title_bounds.RightCenter();
    pos.x -= kTitleButtonSize + kContentMargin;
    pos.y -= kTitleButtonSize / 2;
    pick_button->local_to_parent = SkM44::Translate(pos.x, pos.y);
  }

  RRect CoarseBounds() const override {
    return RRect::MakeSimple(Rect::MakeAtZero({kWidth, height}), kCornerRadius);
  }

  SkPath Shape() const override { return SkPath::RRect(CoarseBounds().sk); }

  void Draw(SkCanvas& canvas) const override {
    auto outer_rrect = CoarseBounds();
    auto border_inner = outer_rrect.Outset(-kBorderWidth);
    SkPaint inner_paint;
    inner_paint.setColor("#c0c0c0"_color);
    canvas.drawRRect(border_inner.sk, inner_paint);
    SkPaint border_paint;
    SetRRectShader(border_paint, border_inner, "#e7e5e2"_color, "#9b9b9b"_color, "#3b3b3b"_color);
    canvas.drawDRRect(outer_rrect.sk, border_inner.sk, border_paint);

    auto contents_rrect = border_inner.Outset(-kContentMargin);
    Rect title_rect = Rect(contents_rrect.rect.left, contents_rrect.rect.top - kTitleHeight,
                           contents_rrect.rect.right, contents_rrect.rect.top);
    SkPaint title_paint;
    SkColor title_colors[] = {"#0654cb"_color, "#030058"_color};
    SkPoint title_points[] = {title_rect.TopLeftCorner(), title_rect.TopRightCorner()};
    title_paint.setShader(
        SkGradientShader::MakeLinear(title_points, title_colors, nullptr, 2, SkTileMode::kClamp));
    canvas.drawRect(title_rect.sk, title_paint);

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override {
    if (btn == gui::PointerButton::Left) {
      auto* location = Closest<Location>(*p.hover);
      auto* machine = Closest<Machine>(*p.hover);
      if (location && machine) {
        auto contact_point = p.PositionWithin(*this);
        auto a = std::make_unique<DragLocationAction>(p, machine->Extract(*location));
        a->contact_point = contact_point;
        return a;
      }
    }
    return nullptr;
  }

  void FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) override {
    children.push_back(pick_button);
  }
};

std::shared_ptr<gui::Widget> Window::MakeWidget() {
  return std::make_shared<WindowWidget>(WeakPtr());
}

}  // namespace automat::library