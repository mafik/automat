// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_toolbar.hh"

#include <include/core/SkMatrix.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>

#include "../build/generated/embedded.hh"
#include "audio.hh"
#include "random.hh"
#include "span.hh"
#include "textures.hh"
#include "widget.hh"
#include "window.hh"

using namespace std::literals;
using namespace maf;

namespace automat::gui {
std::unique_ptr<Action> PrototypeButton::FindAction(gui::Pointer& pointer, gui::ActionTrigger btn) {
  if (btn != gui::PointerButton::Left) {
    return nullptr;
  }
  auto matrix = TransformUp(*pointer.hover, nullptr, &pointer.window.display);
  auto loc = std::make_shared<Location>();
  loc->Create(*proto);
  audio::Play(embedded::assets_SFX_toolbar_pick_wav);
  loc->animation_state.scale = matrix.get(0) / pointer.window.zoom;
  auto contact_point = pointer.PositionWithin(*this);
  loc->position = loc->animation_state.position =
      pointer.PositionWithinRootMachine() - contact_point;
  auto drag_action = std::make_unique<DragLocationAction>(pointer, std::move(loc));
  drag_action->contact_point = contact_point;
  return drag_action;
}
}  // namespace automat::gui

namespace automat::library {

std::shared_ptr<Object> Toolbar::Clone() const {
  auto new_toolbar = std::make_shared<Toolbar>();
  for (const auto& prototype : prototypes) {
    new_toolbar->AddObjectPrototype(prototype);
  }
  return new_toolbar;
}
constexpr float kMarginBetweenIcons = 1_mm;
constexpr float kMarginAroundIcons = 7_mm;
constexpr float kMarginAboveIcons = 8_mm;
constexpr float kToolbarHeight = gui::kToolbarIconSize + kMarginAboveIcons;

SkPath Toolbar::Shape() const {
  float width = CalculateWidth();
  auto rect = Rect(-width / 2, 0, width / 2, kToolbarHeight);
  return SkPath::Rect(rect);
}

static sk_sp<SkImage> ToolbarColor(gui::DrawContext& ctx) {
  return MakeImageFromAsset(embedded::assets_tray_webp, &ctx);
}

animation::Phase Toolbar::Draw(gui::DrawContext& dctx) const {
  float width_targets[buttons.size()];
  for (size_t i = 0; i < buttons.size(); ++i) {
    width_targets[i] = buttons[i]->natural_width;
  }

  auto my_transform = gui::TransformDown(*this, nullptr);

  float width = CalculateWidth();
  int new_hovered_button = -1;
  for (auto* pointer : dctx.display.window->pointers) {
    if (pointer->action) {
      continue;
    }
    Vec2 pointer_position = my_transform.mapPoint(pointer->pointer_position);
    if (pointer_position.x < -width / 2 || pointer_position.x > width / 2) {
      continue;
    }
    if (pointer_position.y > kToolbarHeight) {
      continue;
    }
    float x = -width / 2;
    for (int i = 0; i < buttons.size(); ++i) {
      float button_width = buttons[i]->width;
      if (i == 0) {
        button_width += kMarginAroundIcons;
      } else {
        button_width += kMarginBetweenIcons / 2;
      }
      if (i == buttons.size() - 1) {
        button_width += kMarginAroundIcons;
      } else {
        button_width += kMarginBetweenIcons / 2;
      }
      if (x <= pointer_position.x && pointer_position.x <= x + button_width) {
        width_targets[i] = buttons[i]->natural_width * 2;
        new_hovered_button = i;
        break;
      }
      x += button_width;
    }
  }

  if (hovered_button != new_hovered_button) {
    hovered_button = new_hovered_button;

    static SplitMix64 rng(123);
    static audio::Sound* sounds[3] = {&embedded::assets_SFX_toolbar_select_01_wav,
                                      &embedded::assets_SFX_toolbar_select_02_wav,
                                      &embedded::assets_SFX_toolbar_select_03_wav};

    audio::Play(*sounds[RandomInt<0, 2>(rng)]);
  }

  auto phase = animation::Finished;
  for (int i = 0; i < buttons.size(); ++i) {
    phase |= buttons[i]->width.SineTowards(width_targets[i], dctx.DeltaT(), 0.4);
  }

  auto my_shape = Shape();

  auto color = ToolbarColor(dctx);
  SkRect dst = my_shape.getBounds();
  dctx.canvas.save();
  dctx.canvas.translate(0, kToolbarHeight);
  dctx.canvas.scale(1, -1);
  SkRect left_src = SkRect::MakeLTRB(0, 0, color->height() / 2.f, color->height());
  SkRect left_dst = Rect(dst.left(), 0, dst.left() + kToolbarHeight / 2, kToolbarHeight);
  dctx.canvas.drawImageRect(color, left_src, left_dst, SkSamplingOptions(), nullptr,
                            SkCanvas::kFast_SrcRectConstraint);
  SkRect right_src =
      SkRect::MakeLTRB(color->width() - color->height() / 2.f, 0, color->width(), color->height());
  SkRect right_dst = Rect(dst.right() - kToolbarHeight / 2, 0, dst.right(), kToolbarHeight);
  dctx.canvas.drawImageRect(color, right_src, right_dst, SkSamplingOptions(), nullptr,
                            SkCanvas::kFast_SrcRectConstraint);

  SkRect center_src = SkRect::MakeLTRB(left_src.right(), 0, right_src.left(), color->height());
  SkRect center_dst = Rect(left_dst.right(), 0, right_dst.left(), kToolbarHeight);
  dctx.canvas.drawImageRect(color, center_src, center_dst, SkSamplingOptions(), nullptr,
                            SkCanvas::kFast_SrcRectConstraint);
  dctx.canvas.restore();

  phase |= DrawChildren(dctx);
  return phase;
}

ControlFlow Toolbar::VisitChildren(gui::Visitor& visitor) {
  std::shared_ptr<Widget> arr[buttons.size()];
  for (size_t i = 0; i < buttons.size(); ++i) {
    arr[i] = buttons[i];
  }
  if (auto ret = visitor(maf::SpanOfArr(arr, buttons.size())); ret == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  return ControlFlow::Continue;
}

void Toolbar::AddObjectPrototype(const std::shared_ptr<Object>& new_proto) {
  prototypes.push_back(new_proto->Clone());
  buttons.emplace_back(std::make_shared<gui::PrototypeButton>(prototypes.back()));
  buttons.back()->parent = SharedPtr();
}

SkMatrix Toolbar::TransformToChild(const Widget& child) const {
  float width = CalculateWidth();

  float x = -width / 2 + kMarginAroundIcons;
  for (int i = 0; i < buttons.size(); ++i) {
    if (buttons[i].get() == &child) {
      Rect src = prototypes[i]->CoarseBounds(nullptr).rect;
      float size = buttons[i]->width;
      x += size / 2;
      float scale = buttons[i]->width / buttons[i]->natural_width;
      Rect dst = Rect::MakeWH(size, gui::kToolbarIconSize * scale)
                     .MoveBy({x, gui::kToolbarIconSize * scale / 2});
      auto child_to_parent = SkMatrix::RectToRect(src, dst, SkMatrix::kCenter_ScaleToFit);
      SkMatrix parent_to_child;
      if (child_to_parent.invert(&parent_to_child)) {
        return parent_to_child;
      }
      return SkMatrix::I();
    }
    x += buttons[i]->width + kMarginBetweenIcons;
  }
  return SkMatrix::I();
}

float Toolbar::CalculateWidth() const {
  float width =
      kMarginAroundIcons * 2 + std::max<float>(0, buttons.size() - 1) * kMarginBetweenIcons;
  for (const auto& button : buttons) {
    width += button->width;
  }
  return width;
}
maf::StrView Toolbar::Name() const { return "Toolbar"sv; }
maf::Optional<Rect> Toolbar::TextureBounds(animation::Display*) const {
  float width = CalculateWidth();
  return Rect(-width / 2, 0, width / 2, kToolbarHeight * 2);
}
}  // namespace automat::library