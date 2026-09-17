// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "menu.hpp"

#include <include/core/SkBlurTypes.h>
#include <include/effects/SkImageFilters.h>

#include <memory>
#include <utility>

#include "animation.hpp"
#include "color.hpp"
#include "embedded.hpp"
#include "global_resources.hpp"
#include "math.hpp"
#include "mortal.hpp"
#include "pointer.hpp"
#include "root_widget.hpp"
#include "sincos.hpp"
#include "str.hpp"
#include "textures.hpp"
#include "units.hpp"
#include "widget.hpp"

namespace automat {

PersistentImage kSkyBox = PersistentImage::MakeFromAsset(embedded::assets_skybox_webp);

constexpr float kMenuSize = 2_cm;

void Menu::Place(SinCos angle, Interface option) {
  for (int i = 0; i < slots.size(); ++i) {
    if (slots[i].angle != angle) continue;
    if (option.has_object()) {
      slots[i].option = option;
    } else {
      slots.erase(slots.begin() + i);
    }
    return;
  }
  if (option.has_object()) slots.push_back({angle, option});
}

void Menu::Place(ui::Dir dir, Interface option) {
  Place(SinCos::FromDegrees(static_cast<int>(dir) * 45.f), option);
}

struct MenuAction;

// See: `docs/Bubble Menu, Options & Actions.md`
struct MenuWidget : ui::Widget {
  struct Slot {
    SinCos angle;
    Linked<> option;
    std::unique_ptr<ui::Widget> icon;
    animation::SpringV2<Vec2> offset;
  };
  SmallVec<Slot, 8> slots;
  animation::SpringV2<float> size = 0;
  MortalPtr<MenuAction> action;
  bool first_tick = true;

  MenuWidget(ui::Widget* parent, const Menu& menu, MenuAction* action);

  int PointerSlot(SinCos pointer_dir) const;
  std::unique_ptr<Action> Activate(int slot, ui::Pointer&);

  Optional<Rect> DrawBounds() const override {
    return Rect::MakeAtZero(kMenuSize * 3, kMenuSize * 3);
  }
  Tock Tick(time::Timer& timer) override;
  void Draw(SkCanvas& canvas) const override;
  SkPath Shape() const override { return SkPath::Circle(0, 0, size.value); }
};

struct MenuAction : Action {
  std::unique_ptr<MenuWidget> menu_widget;
  MortalPtr<Toy> toy;
  int last_slot = -1;
  Vec2 last_pos;
  MenuAction(ui::Pointer& pointer) : Action(pointer) {}
  void Update() override {
    auto pos = pointer.PositionWithin(*menu_widget);
    float length = Length(pos);
    int slot = menu_widget->PointerSlot(SinCos::FromVec2(pos, length));
    if (slot == last_slot && (menu_widget->slots.size() == 1 || length > kMenuSize * 2 / 3) &&
        menu_widget->slots[slot].icon) {
      menu_widget->slots[slot].offset.value += pos - last_pos;
    }
    last_slot = slot;
    last_pos = pos;
    if (length > kMenuSize) {
      pointer.ReplaceAction(*this, menu_widget->Activate(slot, pointer));
    }
  }
  ui::Widget* Widget() override { return menu_widget.get(); }
};

MenuWidget::MenuWidget(ui::Widget* parent, const Menu& menu, MenuAction* action)
    : ui::Widget(parent), action(action) {
  auto pos = action->pointer.PositionWithin(*parent);
  local_to_parent = SkM44::Translate(pos.x, pos.y);
  WakeAnimation();
  for (auto& [angle, option] : menu.slots) {
    auto& slot = slots.emplace_back(angle, option, option.MakeIcon(this));
    if (slot.icon) layers.OrderInside(slot.icon.get());
  }
}

int MenuWidget::PointerSlot(SinCos pointer_dir) const {
  int nearest = 0;
  for (int i = 1; i < slots.size(); ++i) {
    if ((pointer_dir - slots[i].angle).cos > (pointer_dir - slots[nearest].angle).cos) nearest = i;
  }
  return nearest;
}

std::unique_ptr<Action> MenuWidget::Activate(int i, ui::Pointer& pointer) {
  auto& slot = slots[i];
  auto option = slot.option.Lock();
  if (!option.has_object()) return nullptr;
  Toy* source = dynamic_cast<Toy*>(slot.icon.get());
  if (source == nullptr && action) source = action->toy.Get();
  if (pointer.root_widget.control->current_state) {
    if (auto drag = option.DragNewController(pointer, *slot.icon)) return drag;
  }
  if (auto sub_menu = option.OpenMenu(pointer, source)) return sub_menu;
  return option.Activate(pointer, source);
}

void MenuWidget::Draw(SkCanvas& canvas) const {
  SkPaint paint = [&]() {
    Status status;
    static auto effect = resources::CompileShader(embedded::assets_bubble_menu_rt_sksl, status);
    assert(effect);

    auto& image = *kSkyBox.image;
    auto dimensions = image->dimensions();

    SkRuntimeEffectBuilder builder(effect);
    builder.uniform("time") = (float)fmod(time::SecondsSinceEpoch(), 1000.0);
    builder.uniform("bubble_radius") = size.value;
    builder.child("environment") = image->makeShader(kDefaultSamplingOptions);
    builder.uniform("environment_size") = SkPoint(dimensions.width(), dimensions.height());

    auto shader = builder.makeShader();
    SkPaint paint;
    paint.setShader(shader);
    return paint;
  }();
  canvas.drawCircle(0, 0, size.value, paint);

  SkPaint shadow_paint;
  shadow_paint.setImageFilter(
      SkImageFilters::DropShadowOnly(0, 0, 0.5_mm, 0.5_mm, "#000000"_color, nullptr));
  auto saved = canvas.getLocalToDevice();
  canvas.saveLayer(nullptr, &shadow_paint);
  for (auto& slot : slots) {
    if (slot.icon == nullptr) continue;
    canvas.setMatrix(saved);
    canvas.concat(slot.icon->local_to_parent);
    canvas.drawDrawable(slot.icon->sk_drawable.get());
  }
  canvas.restore();
  BakeChildren(canvas);
}

ui::Tock MenuWidget::Tick(time::Timer& timer) {
  size.SpringTowards(kMenuSize, timer.d, 0.2, 0.05);
  if (action) {
    Vec2 pos = action->pointer.PositionWithin(*this);
    float length = Length(pos);
    int pointer_slot = PointerSlot(SinCos::FromVec2(pos, length));
    for (int i = 0; i < slots.size(); ++i) {
      if (slots[i].icon == nullptr) continue;
      float r = kMenuSize * 2 / 3;
      auto center = Vec2::Polar(slots[i].angle, r);
      Vec2 target;
      if (i == pointer_slot && (slots.size() == 1 || length > kMenuSize * 2 / 3)) {
        target = pos - center;
      } else {
        target = {0, 0};
      }
      auto& offset = slots[i].offset;
      if (first_tick) {
        offset.value = target;
        offset.velocity = {0, 0};
      } else {
        offset.SineTowards(target, timer.d, 0.3);
      }
    }
    first_tick = false;
  }
  float s = size.value / kMenuSize;

  // Arrange options within the wheel in a visually pleasing way.
  // Scales options to match fit within a given area (arbitrary aspect ratio, divided equally).
  // Ideas:
  //  - scale options to fit an arc segment (makes it easier to control overlap)
  //  - force-directed layout (prevents overlap using physics)
  float bubble_area = kMenuSize * kMenuSize * M_PI;
  float area_per_option = bubble_area / slots.size() / 2;
  for (auto& slot : slots) {
    if (slot.icon == nullptr) continue;
    auto& opt = slot.icon;
    Rect bounds = opt->CoarseBounds().rect;
    float required_area = bounds.Area();

    float scale_to_fit =
        required_area <= area_per_option ? 1 : sqrt(area_per_option / required_area);

    float r = kMenuSize * 2 / 3;
    auto center = Vec2::Polar(slot.angle, r) + slot.offset.value;

    auto desired_size =
        Rect::MakeCenter(center, bounds.Width() * scale_to_fit, bounds.Height() * scale_to_fit);
    opt->local_to_parent = SkM44(
        SkMatrix::RectToRect(bounds, desired_size, SkMatrix::kCenter_ScaleToFit).postScale(s, s));
  }
  return Tock::Drawing;
}

std::unique_ptr<Action> MakeMenuAction(ui::Pointer& pointer, const Menu& menu, Toy* toy) {
  if (menu.slots.empty()) return nullptr;
  auto action = std::make_unique<MenuAction>(pointer);
  action->toy = toy;
  action->menu_widget = std::make_unique<MenuWidget>(pointer.GetWidget(), menu, action.get());
  return action;
}

std::unique_ptr<Action> OptionsProvider::OpenMenu(ui::Pointer& pointer) {
  // TODO: Closest<Toy> is very strange here. OptionsProvider shouldn't have to depend on its
  // parents!
  Toy* toy = ui::Closest<Toy>(static_cast<ui::Widget&>(*this));
  Menu menu;
  FillMenu(pointer, menu);
  return MakeMenuAction(pointer, menu, toy);
}

}  // namespace automat
