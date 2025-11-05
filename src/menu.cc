// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "menu.hh"

#include <include/core/SkBlurTypes.h>
#include <include/effects/SkImageFilters.h>

#include <memory>
#include <utility>

#include "animation.hh"
#include "color.hh"
#include "embedded.hh"
#include "font.hh"
#include "global_resources.hh"
#include "log.hh"
#include "math.hh"
#include "pointer.hh"
#include "root_widget.hh"
#include "sincos.hh"
#include "str.hh"
#include "textures.hh"
#include "units.hh"
#include "vla.hh"
#include "widget.hh"

namespace automat {

std::unique_ptr<ui::Font> kHelsinkiFont = ui::Font::MakeV2(ui::Font::GetHelsinki(), 3_mm);

PersistentImage kSkyBox = PersistentImage::MakeFromAsset(embedded::assets_skybox_webp);

constexpr float kMenuSize = 2_cm;

struct MenuAction;

// Menus have always 8 slots for options.
//
// The plan for menus with more options is to create sub-menus but it's not clear how to approach
// it.
// Option 1 - if a clash happens, alert developers and re-position the options to avoid it (current
// solution)
// Option 2 - copress the extra options into linked-list of sub-menus
//  a) singly-linked
//  b) doubly-linked, where it's possible to go left & right
//  c) tree-like, to minimize the distance to furthest options
//
// It is also possible to track the option usage and figure out which options are more important
// than others and should have priority in menu allocation.
struct MenuWidget : ui::Widget {
  using enum Option::Dir;  // N, S, W, E, NW, NE, SW, SE, DIR_COUNT, DIR_NONE

  struct OptionAnimation {
    animation::SpringV2<Vec2> offset;
  };

  std::unique_ptr<Option> options[DIR_COUNT] = {};
  OptionAnimation option_animation[DIR_COUNT] = {};
  std::unique_ptr<ui::Widget> option_widgets[DIR_COUNT] = {};

  animation::SpringV2<float> size = 0;
  MenuAction* action;
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

  MenuWidget(ui::Widget* parent, Vec<std::unique_ptr<Option>>&& options_vec, MenuAction* action)
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
    for (int i = 0; i < n_opts; ++i) {
      auto preferred_dir = options_vec[i]->PreferredDir();
      if (preferred_dir >= Option::DIR_COUNT) {
        anywhere.Push(i);
        continue;  // can be placed anywhere
      }
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
      bool found_spot = false;
      for (int dist = 1; dist < 5; ++dist) {
        Option::Dir alternative_dir_a = Option::ShiftDir(preferred_dir, dist);
        if (kValidSlots[preferred_mode][alternative_dir_a] &&
            options[alternative_dir_a] == nullptr) {
          options[alternative_dir_a] = std::move(options_vec[i]);
          found_spot = true;
          break;
        }
        Option::Dir alternative_dir_b = Option::ShiftDir(preferred_dir, -dist);
        if (kValidSlots[preferred_mode][alternative_dir_b] &&
            options[alternative_dir_b] == nullptr) {
          options[alternative_dir_b] = std::move(options_vec[i]);
          found_spot = true;
          break;
        }
      }
      if (!found_spot) {
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
      option_widgets[i] = options[i]->MakeIcon(this);
    }
  }
  Option::Dir SinCosToDir(SinCos sc) {
    switch (mode) {
      case MODE_1_DIR:
        return S;
      case MODE_2_DIR:
        return sc.cos >= 0 ? N : S;
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
        float angle = sc.ToDegreesPositive();
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
        float pointer_i_approx = sc.ToDegreesPositive() / 45.f;
        int pointer_i = std::round(pointer_i_approx);
        if (pointer_i >= 8) {
          pointer_i = 0;
        }
        return (Option::Dir)pointer_i;
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
  Optional<Rect> TextureBounds() const override {
    return Rect::MakeAtZero(kMenuSize * 3, kMenuSize * 3);
  }
  animation::Phase Tick(time::Timer& timer) override;
  void FillChildren(Vec<ui::Widget*>& children) override {
    for (auto& opt : option_widgets) {
      if (opt == nullptr) continue;
      children.push_back(opt.get());
    }
  }
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
    for (auto& opt : option_widgets) {
      if (opt == nullptr) continue;
      canvas.setMatrix(saved);
      canvas.concat(opt->local_to_parent);
      canvas.drawDrawable(opt->sk_drawable.get());
    }
    canvas.restore();
    DrawChildren(canvas);
  }
  SkPath Shape() const override { return SkPath::Circle(0, 0, size.value); }
};

struct MenuAction : Action {
  unique_ptr<MenuWidget> menu_widget;
  Option::Dir last_dir = Option::DIR_NONE;
  Vec2 last_pos;
  MenuAction(ui::Pointer& pointer, Vec<std::unique_ptr<Option>>&& options)
      : Action(pointer),
        menu_widget(new MenuWidget(pointer.GetWidget(), std::move(options), this)) {
    auto pos = pointer.PositionWithin(*pointer.GetWidget());
    menu_widget->local_to_parent = SkM44::Translate(pos.x, pos.y);
    menu_widget->WakeAnimation();
  }
  ~MenuAction() override { menu_widget->action = nullptr; }
  void Update() override {
    auto pos = pointer.PositionWithin(*menu_widget);
    float length = Length(pos);
    auto sin_cos = SinCos::FromVec2(pos, length);
    Option::Dir dir = menu_widget->SinCosToDir(sin_cos);
    if (last_dir != Option::DIR_NONE) {
      if (dir == last_dir &&
          ((menu_widget->mode == MenuWidget::MODE_1_DIR) || (length > kMenuSize * 2 / 3))) {
        auto delta = pos - last_pos;
        menu_widget->option_animation[dir].offset.value += delta;
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

animation::Phase MenuWidget::Tick(time::Timer& timer) {
  size.SpringTowards(kMenuSize, timer.d, 0.2, 0.05);
  if (action) {
    Vec2 pos = action->pointer.PositionWithin(*this);
    float length = Length(pos);
    auto pointer_dir = SinCos::FromVec2(pos, length);
    int pointer_i = SinCosToDir(pointer_dir);
    for (int i = 0; i < DIR_COUNT; ++i) {
      if (option_widgets[i] == nullptr) continue;
      auto option_sc = DirToSinCos((Option::Dir)i);
      float r = kMenuSize * 2 / 3;
      auto center = Vec2::Polar(option_sc, r);
      Vec2 target;
      if (i == pointer_i && ((mode == MenuWidget::MODE_1_DIR) || length > kMenuSize * 2 / 3)) {
        target = pos - center;
      } else {
        target = {0, 0};
      }
      auto& anim = option_animation[i];
      if (first_tick) {
        anim.offset.value = target;
        anim.offset.velocity = {0, 0};
      } else {
        anim.offset.SineTowards(target, timer.d, 0.3);
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
    auto& opt = option_widgets[i];
    if (opt == nullptr) continue;
    Rect bounds = opt->CoarseBounds().rect;
    float required_area = bounds.Area();

    float scale_to_fit =
        required_area <= area_per_option ? 1 : sqrt(area_per_option / required_area);

    auto angle = DirToSinCos((Option::Dir)i);
    float r = kMenuSize * 2 / 3;
    auto center = Vec2::Polar(angle, r) + option_animation[i].offset.value;

    auto desired_size =
        Rect::MakeCenter(center, bounds.Width() * scale_to_fit, bounds.Height() * scale_to_fit);
    opt->local_to_parent = SkM44(
        SkMatrix::RectToRect(bounds, desired_size, SkMatrix::kCenter_ScaleToFit).postScale(s, s));

    // opt->local_to_parent = SkM44(SkMatrix::Translate(center - bounds.Center()).postScale(s, s));
  }
  return animation::Animating;
}

Vec<std::unique_ptr<Option>> OptionsProvider::CloneOptions() const {
  Vec<std::unique_ptr<Option>> options;
  VisitOptions([&](Option& opt) { options.push_back(opt.Clone()); });
  return options;
}

std::unique_ptr<Action> OptionsProvider::OpenMenu(ui::Pointer& pointer) const {
  return std::make_unique<MenuAction>(pointer, CloneOptions());
}

struct TextWidget : ui::Widget {
  float width;
  Str text;
  TextWidget(ui::Widget* parent, Str text)
      : ui::Widget(parent), width(kHelsinkiFont->MeasureText(text)), text(text) {}
  Optional<Rect> TextureBounds() const override {
    return Rect(0, -kHelsinkiFont->descent, width, -kHelsinkiFont->ascent);
  }
  SkPath Shape() const override { return SkPath::Rect(*TextureBounds()); }
  void Draw(SkCanvas& canvas) const override {
    if constexpr (false) {  // outline
      SkPaint outline;
      outline.setColor(SK_ColorBLACK);
      outline.setStyle(SkPaint::kStroke_Style);
      outline.setStrokeWidth(1_mm / kHelsinkiFont->font_scale);
      kHelsinkiFont->DrawText(canvas, text, outline);
    }
    if constexpr (false) {  // shadow
      canvas.save();
      SkPaint shadow;
      shadow.setColor(SK_ColorBLACK);
      shadow.setMaskFilter(SkMaskFilter::MakeBlur(SkBlurStyle::kNormal_SkBlurStyle,
                                                  0.5_mm / kHelsinkiFont->font_scale));
      canvas.translate(0, -0.5_mm);
      kHelsinkiFont->DrawText(canvas, text, shadow);
      canvas.restore();
    }
    SkPaint paint;
    paint.setColor(SK_ColorWHITE);
    kHelsinkiFont->DrawText(canvas, text, paint);
  }
};

std::unique_ptr<ui::Widget> TextOption::MakeIcon(ui::Widget* parent) {
  return std::make_unique<TextWidget>(parent, text);
}

}  // namespace automat
