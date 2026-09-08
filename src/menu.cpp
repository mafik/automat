// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "menu.hpp"

#include <include/core/SkBlurTypes.h>
#include <include/effects/SkImageFilters.h>

#include <algorithm>
#include <memory>
#include <utility>

#include "animation.hpp"
#include "color.hpp"
#include "embedded.hpp"
#include "global_resources.hpp"
#include "log.hpp"
#include "math.hpp"
#include "mortal.hpp"
#include "pointer.hpp"
#include "root_widget.hpp"
#include "sincos.hpp"
#include "str.hpp"
#include "text_widget.hpp"
#include "textures.hpp"
#include "units.hpp"
#include "vla.hpp"
#include "widget.hpp"

namespace automat {

Option::~Option() = default;

PersistentImage kSkyBox = PersistentImage::MakeFromAsset(embedded::assets_skybox_webp);

constexpr float kMenuSize = 2_cm;

struct MenuAction;

// See: `docs/Bubble Menu, Options & Actions.md`
struct Menu : ui::Widget {
  using enum Option::Dir;  // E, NE, N, NW, W, SW, S, SE, DIR_COUNT, DIR_NONE

  Ptr<Option> options[DIR_COUNT] = {};
  std::unique_ptr<ui::Widget> icons[DIR_COUNT] = {};
  animation::SpringV2<Vec2> offsets[DIR_COUNT] = {};

  // If some menu option wanted to be placed at position X but had to be moved around, we record
  // that here.
  Option::Dir moved_to_dir[DIR_COUNT] = {DIR_NONE, DIR_NONE, DIR_NONE, DIR_NONE,
                                         DIR_NONE, DIR_NONE, DIR_NONE, DIR_NONE};

  animation::SpringV2<float> size = 0;
  MortalPtr<MenuAction> action;
  bool first_tick = true;

  // Menus with fewer than 8 options may use a compressed display format where only some slots are
  // shown. The options are moved around to fit the reduced number of slots.
  enum MiniMenuMode {
    MODE_8_DIR,  // stardard mode
    MODE_6_DIR,  // N > NE > SE > S > SW > NW (no W & E)
    MODE_4_DIR,  // N > E > S > W
    MODE_2_DIR,  // N > S
    MODE_1_DIR,  // X
  } mode = MODE_8_DIR;

  constexpr static bool kValidSlots[5][DIR_COUNT] = {
      [MODE_8_DIR] = {true, true, true, true, true, true, true, true},
      [MODE_6_DIR] = {false, true, true, true, false, true, true, true},
      [MODE_4_DIR] = {true, false, true, false, true, false, true, false},
      [MODE_2_DIR] = {false, false, true, false, false, false, true, false},
      [MODE_1_DIR] = {false, false, false, false, false, false, true, false},
  };

  Menu(ui::Widget* parent, Vec<Ptr<Option>>&& options_vec, MenuAction* action)
      : ui::Widget(parent), action(action) {
    int n_opts = options_vec.size();

    // Preferred mode is used to shift some options around and try to compress the menu a little
    MiniMenuMode preferred_mode;
    if (n_opts <= 1) {
      preferred_mode = MODE_1_DIR;
    } else if (n_opts <= 2) {
      preferred_mode = MODE_2_DIR;
    } else if (n_opts <= 4) {
      preferred_mode = MODE_4_DIR;
    } else if (n_opts <= 6) {
      preferred_mode = MODE_6_DIR;
    } else {
      preferred_mode = MODE_8_DIR;
    }

    // place options at their preferred positions
    VLA_STACK(anywhere, int, n_opts);
    VLA_STACK(taken, int, n_opts);

    // how many options prefer a given direction?
    int prefers_dir_cnt[DIR_COUNT] = {};

    for (int i = 0; i < n_opts; ++i) {
      auto preferred_dir = options_vec[i]->PreferredDir();
      if (preferred_dir >= Option::DIR_COUNT) {
        anywhere.Push(i);
        continue;  // can be placed anywhere
      }
      prefers_dir_cnt[preferred_dir]++;
      if (!kValidSlots[preferred_mode][preferred_dir]) {
        taken.Push(i);
        continue;  // desired dir doesn't exist in the preferred mode - put it nearby
      }
      if (options[preferred_dir] != nullptr) {
        ERROR_ONCE << "Note to maf: found a menu where two options want the same spot!";
        taken.Push(i);
        continue;  // desired dir is taken - continue
      }
      options[preferred_dir] = std::move(options_vec[i]);
    }

    // TODO: use this list to create a sub-menu
    VLA_STACK(unallocated, int, n_opts);

    for (int i : taken) {
      auto preferred_dir = options_vec[i]->PreferredDir();
      Optional<Option::Dir> spot = std::nullopt;
      for (int dist = 1; dist < 5; ++dist) {
        Option::Dir a = Option::ShiftDir(preferred_dir, dist);
        if (kValidSlots[preferred_mode][a] && options[a] == nullptr) {
          spot = a;
          break;
        }
        Option::Dir b = Option::ShiftDir(preferred_dir, -dist);
        if (kValidSlots[preferred_mode][b] && options[b] == nullptr) {
          spot = b;
          break;
        }
      }
      if (spot.has_value()) {
        if (prefers_dir_cnt[preferred_dir] == 1) {
          // If this is the only option that preferred a given direction, place a "redirect" here.
          // This makes this whole space redirect its events to the original option
          moved_to_dir[preferred_dir] = *spot;
        }
        options[*spot] = std::move(options_vec[i]);
      } else {
        unallocated.Push(i);
      }
    }

    for (int i : anywhere) {
      bool found_spot = false;
      for (int dir = 0; dir < DIR_COUNT; ++dir) {
        if (kValidSlots[preferred_mode][dir] && options[dir] == nullptr) {
          options[dir] = std::move(options_vec[i]);
          found_spot = true;
          break;
        }
      }
      if (!found_spot) {
        unallocated.Push(i);
      }
    }

    if (unallocated) {
      // TODO(maf)
      ERROR_ONCE << "Attempted to display a menu with too many options. " << unallocated.Size()
                 << " options have been dropped. Time to implement sub-menus!";
    }

    if (options[NE] == nullptr && options[E] == nullptr && options[SE] == nullptr &&
        options[NW] == nullptr && options[W] == nullptr && options[SW] == nullptr &&
        options[N] == nullptr) {
      mode = MODE_1_DIR;
    } else if (options[NE] == nullptr && options[E] == nullptr && options[SE] == nullptr &&
               options[NW] == nullptr && options[W] == nullptr && options[SW] == nullptr) {
      mode = MODE_2_DIR;
    } else if (options[NE] == nullptr && options[SE] == nullptr && options[NW] == nullptr &&
               options[NE] == nullptr) {
      mode = MODE_4_DIR;
    } else if (options[E] == nullptr && options[W] == nullptr) {
      mode = MODE_6_DIR;
    }

    for (int i = 0; i < Option::DIR_COUNT; ++i) {
      if (options[i] == nullptr) continue;
      icons[i] = options[i]->MakeIcon(this);
      if (icons[i] == nullptr) {
        ERROR_ONCE << CleanTypeName(typeid(*options[i]).name())
                   << "::MakeIcon returned null. Every option shown in a menu must create an icon "
                      "widget (TextOption gives a text label); this option gets an empty slot.";
        continue;
      }
      layers.OrderInside(icons[i].get());
    }
  }
  Option::Dir SinCosToDir(SinCos sc) {
    float angle = sc.ToDegreesPositive();
    float dir8_float = angle / 45.f;
    int dir8_int = std::round(dir8_float);
    if (dir8_int >= 8) {
      dir8_int = 0;
    }
    if (options[dir8_int] == nullptr && moved_to_dir[dir8_int] != DIR_NONE) {
      return moved_to_dir[dir8_int];
    }
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
        return (Option::Dir)dir8_int;
      }
    }
  }
  SinCos DirToSinCos(Option::Dir dir) {
    if (mode == MODE_6_DIR) {
      switch (dir) {
        case Option::N:
          return 90_deg;
        case Option::NE:
          return 30_deg;
        case Option::SE:
          return 330_deg;
        case Option::S:
          return 270_deg;
        case Option::SW:
          return 210_deg;
        case Option::NW:
          return 150_deg;
        default:
          return 0_deg;
      }
    }
    return SinCos::FromDegrees((float)dir * 45.f);
  }
  int SlotCount() {
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
  Optional<Rect> DrawBounds() const override {
    return Rect::MakeAtZero(kMenuSize * 3, kMenuSize * 3);
  }
  Tock Tick(time::Timer& timer) override;
  void Draw(SkCanvas& canvas) const override {
    auto shape = Shape();

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
  SkPath Shape() const override { return SkPath::Circle(0, 0, size.value); }
};

struct MenuAction : Action {
  unique_ptr<Menu> menu_widget;
  Option::Dir last_dir = Option::DIR_NONE;
  Vec2 last_pos;
  MenuAction(ui::Pointer& pointer, Vec<Ptr<Option>>&& options)
      : Action(pointer), menu_widget(new Menu(pointer.GetWidget(), std::move(options), this)) {
    auto pos = pointer.PositionWithin(*pointer.GetWidget());
    menu_widget->local_to_parent = SkM44::Translate(pos.x, pos.y);
    menu_widget->WakeAnimation();
  }
  void Update() override {
    auto pos = pointer.PositionWithin(*menu_widget);
    float length = Length(pos);
    auto sin_cos = SinCos::FromVec2(pos, length);
    Option::Dir dir = menu_widget->SinCosToDir(sin_cos);
    if (last_dir != Option::DIR_NONE) {
      if (dir == last_dir &&
          ((menu_widget->mode == Menu::MODE_1_DIR) || (length > kMenuSize * 2 / 3))) {
        auto delta = pos - last_pos;
        if (menu_widget->options[dir] != nullptr) {
          menu_widget->offsets[dir].value += delta;
        }
      }
    }
    last_dir = dir;
    last_pos = pos;
    if (length > kMenuSize) {
      auto new_action = menu_widget->options[dir] == nullptr
                            ? nullptr
                            : menu_widget->options[dir]->Activate(pointer);
      pointer.ReplaceAction(*this, std::move(new_action));
    }
  }
  ui::Widget* Widget() override { return menu_widget.get(); }
};

ui::Tock Menu::Tick(time::Timer& timer) {
  size.SpringTowards(kMenuSize, timer.d, 0.2, 0.05);
  if (action) {
    Vec2 pos = action->pointer.PositionWithin(*this);
    float length = Length(pos);
    auto pointer_dir = SinCos::FromVec2(pos, length);
    int pointer_i = SinCosToDir(pointer_dir);
    for (int i = 0; i < DIR_COUNT; ++i) {
      if (options[i] == nullptr) continue;
      auto option_sc = DirToSinCos((Option::Dir)i);
      float r = kMenuSize * 2 / 3;
      auto center = Vec2::Polar(option_sc, r);
      Vec2 target;
      if (i == pointer_i && ((mode == Menu::MODE_1_DIR) || length > kMenuSize * 2 / 3)) {
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
  float area_per_option = bubble_area / SlotCount() / 2;
  for (int i = 0; i < DIR_COUNT; ++i) {
    if (icons[i] == nullptr) continue;
    auto& opt = icons[i];
    Rect bounds = opt->CoarseBounds().rect;
    float required_area = bounds.Area();

    float scale_to_fit =
        required_area <= area_per_option ? 1 : sqrt(area_per_option / required_area);

    auto angle = DirToSinCos((Option::Dir)i);
    float r = kMenuSize * 2 / 3;
    auto center = Vec2::Polar(angle, r) + offsets[i].value;

    auto desired_size =
        Rect::MakeCenter(center, bounds.Width() * scale_to_fit, bounds.Height() * scale_to_fit);
    opt->local_to_parent = SkM44(
        SkMatrix::RectToRect(bounds, desired_size, SkMatrix::kCenter_ScaleToFit).postScale(s, s));
  }
  return Tock::Drawing;
}

std::unique_ptr<Action> OptionsProvider::TriggerActivate(ui::Pointer& pointer,
                                                         ui::ActionTrigger trigger) {
  std::unique_ptr<Action> action;
  auto callback = [&](Option& option) {
    if (std::ranges::contains(option.Triggers(), trigger)) action = option.Activate(pointer);
    return action ? LoopControl::Break : LoopControl::Continue;
  };
  OptionVisitor visit{callback};
  Options(pointer, visit);
  return action;
}

std::unique_ptr<Action> OptionsProvider::OpenMenu(ui::Pointer& pointer) {
  Vec<Ptr<Option>> options;
  auto callback = [&](Option& option) {
    if (auto clone = option.Clone()) options.push_back(std::move(clone));
    return LoopControl::Continue;
  };
  OptionVisitor visit{callback};
  Options(pointer, visit);
  if (options.empty()) return nullptr;
  return std::make_unique<MenuAction>(pointer, std::move(options));
}

TextOption::TextOption(Str text) : text(text) {}

std::unique_ptr<ui::Widget> TextOption::MakeIcon(ui::Widget* parent) {
  return std::make_unique<TextWidget>(parent, text);
}

}  // namespace automat
