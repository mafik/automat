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

using ui::kDirCount;

constexpr bool kValidSlots[5][kDirCount] = {
    [MODE_8_DIR] = {true, true, true, true, true, true, true, true},
    [MODE_6_DIR] = {false, true, true, true, false, true, true, true},
    [MODE_4_DIR] = {true, false, true, false, true, false, true, false},
    [MODE_2_DIR] = {false, false, true, false, false, false, true, false},
    [MODE_1_DIR] = {false, false, false, false, false, false, true, false},
};

static ui::Dir SinCosToDir(MiniMenuMode mode, SinCos sc) {
  using enum ui::Dir;
  float angle = sc.ToDegreesPositive();
  switch (mode) {
    case MODE_1_DIR:
      return S;
    case MODE_2_DIR:
      return sc.sin >= 0 ? N : S;
    case MODE_4_DIR:
      if (sc.cos > Fixed1(0.7071)) {
        return E;
      } else if (sc.cos < Fixed1(-0.7071)) {
        return W;
      } else if (sc.sin > 0) {
        return N;
      } else {
        return S;
      }
    case MODE_6_DIR: {
      if (angle < 60) {
        return NE;
      } else if (angle < 120) {
        return N;
      } else if (angle < 180) {
        return NW;
      } else if (angle < 240) {
        return SW;
      } else if (angle < 300) {
        return S;
      } else {
        return SE;
      }
    }
    case MODE_8_DIR: {
      int dir8 = std::round(angle / 45.f);
      return static_cast<ui::Dir>(dir8 >= 8 ? 0 : dir8);
    }
  }
}

static SinCos DirToSinCos(MiniMenuMode mode, ui::Dir dir) {
  using enum ui::Dir;
  if (mode == MODE_6_DIR) {
    switch (dir) {
      case N:
        return 90_deg;
      case NE:
        return 30_deg;
      case SE:
        return 330_deg;
      case S:
        return 270_deg;
      case SW:
        return 210_deg;
      case NW:
        return 150_deg;
      default:
        return 0_deg;
    }
  }
  return SinCos::FromDegrees(static_cast<int>(dir) * 45.f);
}

static int SlotCount(MiniMenuMode mode) {
  switch (mode) {
    case MODE_8_DIR:
      return 8;
    case MODE_6_DIR:
      return 6;
    case MODE_4_DIR:
      return 4;
    case MODE_2_DIR:
      return 2;
    case MODE_1_DIR:
      return 1;
  }
}

struct MenuAction;

// See: `docs/Bubble Menu, Options & Actions.md`
struct Menu : ui::Widget {
  MiniMenuMode mode = MODE_8_DIR;
  Linked<> slots[kDirCount];
  std::unique_ptr<ui::Widget> icons[kDirCount];
  animation::SpringV2<Vec2> offsets[kDirCount];
  animation::SpringV2<float> size = 0;
  MortalPtr<MenuAction> action;
  bool first_tick = true;

  Menu(ui::Widget* parent, MenuAction* action);
  Menu(ui::Widget* parent, MiniMenuMode mode, const Interface (&options)[kDirCount],
       MenuAction* action);

  ui::Dir PointerDir(SinCos sc) const { return SinCosToDir(mode, sc); }
  std::unique_ptr<Action> Activate(int dir, ui::Pointer&);

  Optional<Rect> DrawBounds() const override {
    return Rect::MakeAtZero(kMenuSize * 3, kMenuSize * 3);
  }
  Tock Tick(time::Timer& timer) override;
  void Draw(SkCanvas& canvas) const override;
  SkPath Shape() const override { return SkPath::Circle(0, 0, size.value); }
};

struct MenuAction : Action {
  std::unique_ptr<Menu> menu_widget;
  MortalPtr<Toy> toy;
  int last_dir = -1;
  Vec2 last_pos;
  MenuAction(ui::Pointer& pointer) : Action(pointer) {}
  void Update() override {
    auto pos = pointer.PositionWithin(*menu_widget);
    float length = Length(pos);
    int dir = static_cast<int>(menu_widget->PointerDir(SinCos::FromVec2(pos, length)));
    if (dir == last_dir && (menu_widget->mode == MODE_1_DIR || length > kMenuSize * 2 / 3) &&
        menu_widget->icons[dir]) {
      menu_widget->offsets[dir].value += pos - last_pos;
    }
    last_dir = dir;
    last_pos = pos;
    if (length > kMenuSize) {
      pointer.ReplaceAction(*this, menu_widget->Activate(dir, pointer));
    }
  }
  ui::Widget* Widget() override { return menu_widget.get(); }
};

Menu::Menu(ui::Widget* parent, MenuAction* action) : ui::Widget(parent), action(action) {
  auto pos = action->pointer.PositionWithin(*parent);
  local_to_parent = SkM44::Translate(pos.x, pos.y);
  WakeAnimation();
}

Menu::Menu(ui::Widget* parent, MiniMenuMode mode, const Interface (&options)[kDirCount],
           MenuAction* action)
    : Menu(parent, action) {
  this->mode = mode;
  for (int i = 0; i < kDirCount; ++i) {
    if (!options[i].has_object()) continue;
    slots[i] = options[i];
    icons[i] = options[i].MakeIcon(this);
    if (icons[i]) layers.OrderInside(icons[i].get());
  }
}

std::unique_ptr<Action> Menu::Activate(int dir, ui::Pointer& pointer) {
  auto option = slots[dir].Lock();
  if (!option.has_object()) return nullptr;
  Toy* source = dynamic_cast<Toy*>(icons[dir].get());
  if (source == nullptr && action) source = action->toy.Get();
  if (pointer.root_widget.control->current_state) {
    if (auto drag = option.DragNewController(pointer, *icons[dir])) return drag;
  }
  if (auto sub_menu = option.OpenMenu(pointer, source)) return sub_menu;
  return option.Activate(pointer, source);
}

void Menu::Draw(SkCanvas& canvas) const {
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
  for (auto& icon : icons) {
    if (icon == nullptr) continue;
    canvas.setMatrix(saved);
    canvas.concat(icon->local_to_parent);
    canvas.drawDrawable(icon->sk_drawable.get());
  }
  canvas.restore();
  BakeChildren(canvas);
}

ui::Tock Menu::Tick(time::Timer& timer) {
  size.SpringTowards(kMenuSize, timer.d, 0.2, 0.05);
  if (action) {
    Vec2 pos = action->pointer.PositionWithin(*this);
    float length = Length(pos);
    auto pointer_dir = SinCos::FromVec2(pos, length);
    int pointer_i = static_cast<int>(PointerDir(pointer_dir));
    for (int i = 0; i < kDirCount; ++i) {
      if (icons[i] == nullptr) continue;
      auto option_sc = DirToSinCos(mode, static_cast<ui::Dir>(i));
      float r = kMenuSize * 2 / 3;
      auto center = Vec2::Polar(option_sc, r);
      Vec2 target;
      if (i == pointer_i && ((mode == MODE_1_DIR) || length > kMenuSize * 2 / 3)) {
        target = pos - center;
      } else {
        target = {0, 0};
      }
      auto& offset = offsets[i];
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
  float area_per_option = bubble_area / SlotCount(mode) / 2;
  for (int i = 0; i < kDirCount; ++i) {
    if (icons[i] == nullptr) continue;
    auto& opt = icons[i];
    Rect bounds = opt->CoarseBounds().rect;
    float required_area = bounds.Area();

    float scale_to_fit =
        required_area <= area_per_option ? 1 : sqrt(area_per_option / required_area);

    auto angle = DirToSinCos(mode, static_cast<ui::Dir>(i));
    float r = kMenuSize * 2 / 3;
    auto center = Vec2::Polar(angle, r) + offsets[i].value;

    auto desired_size =
        Rect::MakeCenter(center, bounds.Width() * scale_to_fit, bounds.Height() * scale_to_fit);
    opt->local_to_parent = SkM44(
        SkMatrix::RectToRect(bounds, desired_size, SkMatrix::kCenter_ScaleToFit).postScale(s, s));
  }
  return Tock::Drawing;
}

std::unique_ptr<Action> MakeMenuAction(ui::Pointer& pointer, MiniMenuMode mode,
                                       const Interface (&options)[kDirCount], Toy* toy) {
  Interface valid[kDirCount];
  bool found = false;
  for (int i = 0; i < kDirCount; ++i) {
    if (!kValidSlots[mode][i]) continue;
    valid[i] = options[i];
    found |= options[i].has_object();
  }
  if (!found) return nullptr;
  auto action = std::make_unique<MenuAction>(pointer);
  action->toy = toy;
  action->menu_widget = std::make_unique<Menu>(pointer.GetWidget(), mode, valid, action.get());
  return action;
}

std::unique_ptr<Action> OptionsProvider::OpenMenu(ui::Pointer& pointer) {
  // TODO: Closest<Toy> is very strange here. OptionsProvider shouldn't have to depend on its
  // parents!
  Toy* toy = ui::Closest<Toy>(static_cast<ui::Widget&>(*this));
  Interface options[kDirCount];
  for (int i = 0; i < kDirCount; ++i) {
    options[i] = FindOption(pointer, static_cast<ui::Dir>(i));
  }
  return MakeMenuAction(pointer, MenuMode(), options, toy);
}

}  // namespace automat
