// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "menu.hh"

#include <include/effects/SkImageFilters.h>

#include <memory>

#include "animation.hh"
#include "color.hh"
#include "embedded.hh"
#include "font.hh"
#include "global_resources.hh"
#include "math.hh"
#include "pointer.hh"
#include "str.hh"
#include "textures.hh"
#include "units.hh"
#include "widget.hh"

using namespace maf;

namespace automat {

std::unique_ptr<gui::Font> kHelsinkiFont = gui::Font::MakeV2(gui::Font::GetHelsinki(), 3_mm);

PersistentImage kSkyBox = PersistentImage::MakeFromAsset(embedded::assets_skybox_webp);

constexpr float kMenuSize = 2_cm;

struct MenuAction;

struct MenuWidget : gui::Widget {
  struct OptionAnimation {
    animation::SpringV2<Vec2> offset;
  };

  Vec<std::unique_ptr<Option>> options;
  Vec<OptionAnimation> option_animation;
  animation::SpringV2<float> size = 0;
  MenuAction* action;
  bool first_tick = true;

  MenuWidget(Vec<std::unique_ptr<Option>>&& options, MenuAction* action)
      : options(std::move(options)), action(action) {
    option_animation.resize(this->options.size());
  }
  Optional<Rect> TextureBounds() const override {
    return Rect::MakeAtZero(kMenuSize * 3, kMenuSize * 3);
  }
  animation::Phase Tick(time::Timer& timer) override;
  void FillChildren(maf::Vec<Ptr<gui::Widget>>& children) override {
    for (auto& opt : options) {
      children.emplace_back(opt->icon);
    }
  }
  void Draw(SkCanvas& canvas) const override {
    auto shape = Shape();

    SkPaint paint = [&]() {
      static auto builder =
          resources::RuntimeEffectBuilder(embedded::assets_bubble_menu_rt_sksl.content);

      auto& image = *kSkyBox.image;
      auto dimensions = image->dimensions();

      builder->uniform("time") = (float)fmod(time::SteadyNow().time_since_epoch().count(), 1000.0);
      builder->uniform("bubble_radius") = size.value;
      builder->child("environment") = image->makeShader(kDefaultSamplingOptions);
      builder->uniform("environment_size") = SkPoint(dimensions.width(), dimensions.height());

      auto shader = builder->makeShader();
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
    for (auto& opt : options) {
      canvas.setMatrix(saved);
      canvas.concat(opt->icon->local_to_parent);
      canvas.drawDrawable(opt->icon->sk_drawable.get());
    }
    canvas.restore();
    DrawChildren(canvas);
  }
  SkPath Shape() const override { return SkPath::Circle(0, 0, size.value); }
};

struct MenuAction : Action {
  Ptr<MenuWidget> menu_widget;
  int last_index = -1;
  Vec2 last_pos;
  MenuAction(gui::Pointer& pointer, Vec<std::unique_ptr<Option>>&& options)
      : Action(pointer), menu_widget(MakePtr<MenuWidget>(std::move(options), this)) {
    auto pos = pointer.PositionWithin(*pointer.GetWidget());
    menu_widget->local_to_parent = SkM44::Translate(pos.x, pos.y);
    menu_widget->WakeAnimation();
  }
  ~MenuAction() override { menu_widget->action = nullptr; }
  void Update() override {
    int n_opts = menu_widget->options.size();
    auto pos = pointer.PositionWithin(*menu_widget);
    float length = Length(pos);
    auto dir = SinCos::FromVec2(pos, length);
    float index_approx = dir.ToDegreesPositive() / 360.0f * n_opts;
    int index = std::round(index_approx);
    if (index >= n_opts) {
      index = 0;
    }
    if (last_index != -1 && n_opts > 0) {
      if (index == last_index && ((n_opts == 1) || (length > kMenuSize * 2 / 3))) {
        auto delta = pos - last_pos;
        menu_widget->option_animation[index].offset.value += delta;
      }
    }
    last_index = index;
    last_pos = pos;
    if (length > kMenuSize) {
      auto new_action = n_opts == 0 ? nullptr : menu_widget->options[index]->Activate(pointer);
      pointer.ReplaceAction(*this, std::move(new_action));
    }
  }
  gui::Widget* Widget() override { return menu_widget.get(); }
};

animation::Phase MenuWidget::Tick(time::Timer& timer) {
  size.SpringTowards(kMenuSize, timer.d, 0.2, 0.05);
  int n_opts = options.size();
  if (action) {
    Vec2 pos = action->pointer.PositionWithin(*this);
    float length = Length(pos);
    auto pointer_dir = SinCos::FromVec2(pos, length);
    float pointer_i_approx = pointer_dir.ToDegreesPositive() / 360.0f * n_opts;
    int pointer_i = std::round(pointer_i_approx);
    if (pointer_i >= n_opts) {
      pointer_i = 0;
    }
    for (int i = 0; i < n_opts; ++i) {
      auto option_dir = SinCos::FromRadians(M_PI * 2 * i / n_opts);
      float r = kMenuSize * 2 / 3;
      auto center = Vec2::Polar(option_dir, r);
      Vec2 target;
      if (i == pointer_i && (n_opts <= 1 || length > kMenuSize * 2 / 3)) {
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
  for (int i = 0; i < n_opts; ++i) {
    auto& opt = options[i];
    Rect bounds = opt->icon->Shape().getBounds();
    auto dir = SinCos::FromRadians(M_PI * 2 * i / n_opts);
    float r = kMenuSize * 2 / 3;
    auto center = Vec2::Polar(dir, r) + option_animation[i].offset.value;
    opt->icon->local_to_parent =
        SkM44(SkMatrix::Translate(center - bounds.Center()).postScale(s, s));
  }
  return animation::Animating;
}

maf::Vec<std::unique_ptr<Option>> OptionsProvider::CloneOptions() const {
  maf::Vec<std::unique_ptr<Option>> options;
  VisitOptions([&](Option& opt) { options.push_back(opt.Clone()); });
  return options;
}

std::unique_ptr<Action> OptionsProvider::OpenMenu(gui::Pointer& pointer) const {
  return std::make_unique<MenuAction>(pointer, CloneOptions());
}

struct TextWidget : gui::Widget {
  float width;
  Str text;
  TextWidget(Str text) : width(kHelsinkiFont->MeasureText(text)), text(text) {}
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

Option::Option(Ptr<gui::Widget>&& icon) : icon(icon) {}

Option::Option(Str name) : icon(MakePtr<TextWidget>(name)) {}

}  // namespace automat