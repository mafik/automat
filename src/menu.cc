// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "menu.hh"

#include "embedded.hh"
#include "font.hh"
#include "global_resources.hh"
#include "pointer.hh"
#include "textures.hh"
#include "units.hh"
#include "widget.hh"

using namespace maf;

namespace automat {

PersistentImage kSkyBox = PersistentImage::MakeFromAsset(embedded::assets_skybox_webp);

constexpr float kMenuSize = 2_cm;

struct MenuWidget : gui::Widget {
  Vec<std::unique_ptr<Option>> options;
  animation::SpringV2<float> size = 0;
  animation::Phase Tick(time::Timer& timer) override {
    size.SpringTowards(kMenuSize, timer.d, 0.2, 0.05);
    return animation::Animating;
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
    // paint.
    canvas.drawPath(shape, paint);

    auto& font = gui::GetFont();
    int n_opts = options.size();
    for (int i = 0; i < n_opts; ++i) {
      auto& opt = options[i];
      auto dir = SinCos::FromRadians(M_PI * 2 * i / n_opts);
      auto name = opt->Name();
      float w = font.MeasureText(name);
      float r = size.value * 0.5;
      auto pos = Vec2::Polar(dir, r) + Vec2(w * -0.5f, font.letter_height * -0.5f);
      canvas.save();
      canvas.translate(pos.x, pos.y);
      font.DrawText(canvas, name, SkPaint());
      canvas.restore();
    }
  }
  SkPath Shape() const override { return SkPath::Circle(0, 0, size.value); }
};

struct MenuAction : Action {
  std::shared_ptr<MenuWidget> menu_widget;
  MenuAction(gui::Pointer& pointer) : Action(pointer), menu_widget(std::make_shared<MenuWidget>()) {
    auto pos = pointer.PositionWithin(*pointer.GetWidget());
    menu_widget->local_to_parent = SkM44::Translate(pos.x, pos.y);
    menu_widget->WakeAnimation();
  }
  void Update() override {
    auto pos = pointer.PositionWithin(*menu_widget);
    float length = Length(pos);
    if (length > kMenuSize) {
      int n_opts = menu_widget->options.size();
      auto dir = SinCos::FromVec2(pos, length);
      float index_approx = dir.ToDegreesPositive() / 360.0f * n_opts;
      int index = std::round(index_approx);
      if (index >= n_opts) {
        index = 0;
      }
      pointer.ReplaceAction(*this, menu_widget->options[index]->Activate(pointer));
    }
  }
  gui::Widget* Widget() override { return menu_widget.get(); }
};

std::unique_ptr<Action> OpenMenu(gui::Pointer& pointer,
                                 maf::Vec<std::unique_ptr<Option>>&& options) {
  auto menu = std::make_unique<MenuAction>(pointer);
  menu->menu_widget->options = std::move(options);
  return menu;
}

}  // namespace automat