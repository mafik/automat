// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_toolbar.hh"

#include <include/core/SkMatrix.h>
#include <include/core/SkRect.h>
#include <include/core/SkSamplingOptions.h>

#include "../build/generated/embedded.hh"
#include "audio.hh"
#include "automat.hh"
#include "random.hh"
#include "root_widget.hh"
#include "textures.hh"
#include "widget.hh"

using namespace std::literals;

namespace automat::ui {

void PrototypeButton::Init() {
  proto_widget = &ToyStore().FindOrMake(*proto, this);
  auto rect = proto_widget->CoarseBounds().rect;
  natural_width =
      std::min<float>(kToolbarIconSize, rect.Width() * kToolbarIconSize / rect.Height());
  width.value = natural_width;
}

std::unique_ptr<Action> PrototypeButton::FindAction(ui::Pointer& pointer, ui::ActionTrigger btn) {
  if (btn != ui::PointerButton::Left) {
    return nullptr;
  }
  auto loc = MAKE_PTR(Location, root_location->AcquireWeakPtr());
  loc->Create(*proto);
  pointer.root_widget.toys.FindOrMake(*loc, this);
  audio::Play(embedded::assets_SFX_toolbar_pick_wav);
  return std::make_unique<DragLocationAction>(pointer, std::move(loc));
}

constexpr float kMarginBetweenIcons = 1_mm;
constexpr float kMarginAroundIcons = 7_mm;
constexpr float kMarginAboveIcons = 8_mm;
constexpr float kToolbarHeight = ui::kToolbarIconSize + kMarginAboveIcons;

SkPath Toolbar::Shape() const {
  float width = CalculateWidth();
  auto rect = Rect(-width / 2, 0, width / 2, kToolbarHeight);
  return SkPath::Rect(rect);
}

animation::Phase Toolbar::Tick(time::Timer& timer) {
  float width_targets[buttons.size()];
  for (size_t i = 0; i < buttons.size(); ++i) {
    width_targets[i] = buttons[i]->natural_width;
  }

  auto my_transform = ui::TransformDown(*this);

  float width = CalculateWidth();
  int new_hovered_button = -1;
  auto& root_widget = FindRootWidget();

  for (auto* pointer : root_widget.pointers) {
    if (pointer->actions[static_cast<int>(PointerButton::Left)]) {
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
    phase |= buttons[i]->width.SineTowards(width_targets[i], timer.d, 0.4);
  }
  UpdateChildTransform();
  return phase;
}

void Toolbar::Draw(SkCanvas& canvas) const {
  auto my_shape = Shape();

  static auto color = PersistentImage::MakeFromAsset(embedded::assets_tray_webp, {.scale = 1});
  SkRect dst = my_shape.getBounds();
  canvas.save();
  canvas.translate(0, kToolbarHeight);
  canvas.scale(1, -1);
  SkRect left_src = SkRect::MakeLTRB(0, 0, color.heightPx() / 2.f, color.heightPx());
  SkRect left_dst = Rect(dst.left(), 0, dst.left() + kToolbarHeight / 2, kToolbarHeight);
  canvas.drawImageRect(*color.image, left_src, left_dst, SkSamplingOptions(), nullptr,
                       SkCanvas::kFast_SrcRectConstraint);
  SkRect right_src = SkRect::MakeLTRB(color.widthPx() - color.heightPx() / 2.f, 0, color.widthPx(),
                                      color.heightPx());
  SkRect right_dst = Rect(dst.right() - kToolbarHeight / 2, 0, dst.right(), kToolbarHeight);
  canvas.drawImageRect(*color.image, right_src, right_dst, SkSamplingOptions(), nullptr,
                       SkCanvas::kFast_SrcRectConstraint);

  SkRect center_src = SkRect::MakeLTRB(left_src.right(), 0, right_src.left(), color.heightPx());
  SkRect center_dst = Rect(left_dst.right(), 0, right_dst.left(), kToolbarHeight);
  canvas.drawImageRect(*color.image, center_src, center_dst, SkSamplingOptions(), nullptr,
                       SkCanvas::kFast_SrcRectConstraint);
  canvas.restore();

  DrawChildren(canvas);
}

void Toolbar::FillChildren(Vec<Widget*>& children) {
  children.reserve(buttons.size());
  for (size_t i = 0; i < buttons.size(); ++i) {
    children.push_back(buttons[i].get());
  }
}

void Toolbar::AddObjectPrototype(const Ptr<Object>& new_proto) {
  prototypes.push_back(new_proto->Clone());
  buttons.emplace_back(std::make_unique<ui::PrototypeButton>(this, prototypes.back()));
  if (auto widget = dynamic_cast<Widget*>(prototypes.back().Get())) {
    widget->parent = buttons.back().get();
  }
  buttons.back()->Init();
}

void Toolbar::UpdateChildTransform() {
  float width = CalculateWidth();

  float x = -width / 2 + kMarginAroundIcons;
  for (int i = 0; i < buttons.size(); ++i) {
    Rect src = buttons[i]->CoarseBounds().rect;
    float size = buttons[i]->width;
    float x2 = x + size / 2;
    float scale = buttons[i]->width / buttons[i]->natural_width;
    Rect dst = Rect::MakeCenterZero(size, ui::kToolbarIconSize * scale)
                   .MoveBy({x2, ui::kToolbarIconSize * scale / 2});
    buttons[i]->local_to_parent =
        SkM44(SkMatrix::RectToRect(src, dst, SkMatrix::kCenter_ScaleToFit));
    x += buttons[i]->width + kMarginBetweenIcons;
  }
}

float Toolbar::CalculateWidth() const {
  float width =
      kMarginAroundIcons * 2 + std::max<float>(0, buttons.size() - 1) * kMarginBetweenIcons;
  for (const auto& button : buttons) {
    width += button->width;
  }
  return width;
}
StrView Toolbar::Name() const { return "Toolbar"sv; }
Optional<Rect> Toolbar::TextureBounds() const {
  float width = CalculateWidth();
  return Rect(-width / 2, 0, width / 2, kToolbarHeight * 2);
}

}  // namespace automat::ui
