// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_window.hh"

#include <include/core/SkTileMode.h>
#include <include/effects/SkGradientShader.h>

#include <ranges>

#include "drawing.hh"
#include "font.hh"
#include "gui_button.hh"
#include "gui_shape_widget.hh"
#include "key.hh"
#include "pointer.hh"
#include "root_widget.hh"
#include "svg.hh"

#ifdef __linux__
#include "xcb.hh"
#endif

using namespace maf;

namespace automat::library {

constexpr bool kDebugWindowPicking = false;

Window::Window() {}

std::string_view Window::Name() const { return "Window"; }

std::shared_ptr<Object> Window::Clone() const { return std::make_shared<Window>(*this); }

const SkMatrix kCenterPickIcon = SkMatrix::Translate(-1_mm, 0);

constexpr static float kBorderWidth = 0.5_mm;    // width of the border
constexpr static float kContentMargin = 0.5_mm;  // margin between the border and the content
constexpr static float kTitleHeight = 8_mm;      // height of the title bar
constexpr static float kTitleButtonSize = kTitleHeight - 2 * kContentMargin;

struct WindowWidget;

struct PickButton : gui::Button {
  std::function<void(gui::Pointer&)> on_activate;
  PickButton() : gui::Button(gui::MakeShapeWidget(kPickSVG, "#000000"_color, &kCenterPickIcon)) {}
  SkRRect RRect() const override {
    return RRect::MakeSimple(
               Rect::MakeAtZero<::LeftX, ::BottomY>({kTitleButtonSize, kTitleButtonSize}), 1_mm)
        .sk;
  }
  SkColor BackgroundColor() const override { return "#d0d0d0"_color; }

  void Activate(gui::Pointer& p) override {
    WakeAnimation();
    on_activate(p);
  }
};

struct WindowWidget : gui::Widget, gui::PointerGrabber, gui::KeyGrabber {
  std::weak_ptr<Window> window_weak;

  constexpr static float kWidth = 5_cm;
  constexpr static float kCornerRadius = 1_mm;
  constexpr static float kDefaultHeight = 5_cm;

  float height = kDefaultHeight;
  gui::PointerGrab* pointer_grab = nullptr;
  gui::KeyGrab* key_grab = nullptr;

  std::shared_ptr<PickButton> pick_button;
  std::string window_name;

  WindowWidget(std::weak_ptr<Window>&& window) : window_weak(std::move(window)) {
    pick_button = std::make_shared<PickButton>();
    pick_button->on_activate = [this](gui::Pointer& p) {
      p.EndAction();
      pointer_grab = &p.RequestGlobalGrab(*this);
      key_grab = &p.keyboard->RequestKeyGrab(*this, gui::AnsiKey::Escape, false, false, false,
                                             false, [this](maf::Status& status) {
                                               if (pointer_grab) pointer_grab->Release();
                                               if (key_grab) key_grab->Release();
                                             });
    };
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

    auto& font = gui::GetFont();
    SkPaint title_text_paint;
    title_text_paint.setColor("#ffffff"_color);
    canvas.save();
    auto title_text_pos = title_rect.LeftCenter();
    title_text_pos.x += kContentMargin;
    title_text_pos.y -= font.letter_height / 2;
    canvas.translate(title_text_pos.x, title_text_pos.y);
    font.DrawText(canvas, window_name, title_text_paint);
    canvas.restore();

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

  void ReleaseGrab(gui::PointerGrab&) override { pointer_grab = nullptr; }
  void ReleaseKeyGrab(gui::KeyGrab&) override { key_grab = nullptr; }
  void KeyGrabberKeyDown(gui::KeyGrab&) override {
    if (pointer_grab) pointer_grab->Release();
    if (key_grab) key_grab->Release();
  }
  void PointerGrabberButtonDown(gui::PointerGrab& grab, gui::PointerButton) override {
#ifdef __linux__
    grab.Release();

    xcb_window_t picked_window = XCB_WINDOW_NONE;
    {
      auto query_pointer_reply = xcb::query_pointer();
      picked_window = query_pointer_reply->child;
    }

    if (kDebugWindowPicking) {
      LOG << "Picked window: " << f("%x", picked_window);
      LOG_Indent();
    }

    // Find the first window that has WM_STATE
    xcb_window_t found_window = XCB_WINDOW_NONE;
    std::deque<xcb_window_t> search_list;
    search_list.push_back(picked_window);
    while (!search_list.empty()) {
      // Pick the front window, search its children and if none of them has WM_STATE, add them to
      // the queue.
      auto curr = search_list.front();
      search_list.pop_front();
      if (curr == XCB_WINDOW_NONE) {
        continue;
      }

      if (kDebugWindowPicking) {
        LOG << "Checking for WM_STATE: " << f("%x", curr);
      }
      auto property_reply = xcb::get_property(curr, xcb::atom::_NET_WM_STATE, XCB_ATOM_ANY, 0, 0);
      if (property_reply->type != XCB_ATOM_NONE) {
        found_window = curr;
        if (kDebugWindowPicking) {
          LOG << "Found!";
        }
        break;
      }
      auto query_tree_reply = xcb::query_tree(curr);
      if (kDebugWindowPicking) {
        LOG << "Contains " << query_tree_reply->children_len << " children";
        LOG_Indent();
      }
      for (auto child : std::ranges::reverse_view(xcb::query_tree_children(*query_tree_reply))) {
        search_list.push_back(child);
      }
      if (kDebugWindowPicking) {
        LOG_Unindent();
      }
    }
    if (kDebugWindowPicking) {
      if (found_window != XCB_WINDOW_NONE) {
        auto name = xcb::GetPropertyString(found_window, XCB_ATOM_WM_NAME);
        LOG << "Found window: " << f("%x", found_window) << " " << name;
      } else {
        LOG << "No window found";
      }
      LOG_Unindent();
    }
    window_name = xcb::GetPropertyString(found_window, XCB_ATOM_WM_NAME);
    WakeAnimation();
    if (auto window = window_weak.lock()) {
      window->xcb_window = found_window;
    }
#else
    grab.Release();
#endif
  }
};

std::shared_ptr<gui::Widget> Window::MakeWidget() {
  return std::make_shared<WindowWidget>(WeakPtr());
}

}  // namespace automat::library