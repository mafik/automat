// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_hotkey.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkShader.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>

#include <chrono>
#include <memory>

#include "arcline.hh"
#include "color.hh"
#include "gui_constants.hh"
#include "key_button.hh"
#include "keyboard.hh"
#include "library_macros.hh"
#include "math.hh"
#include "text_field.hh"
#include "widget.hh"

#if defined(__linux__)
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>
#endif

using namespace automat::gui;
using namespace maf;

namespace automat::library {

DEFINE_PROTO(HotKey);

static constexpr float kCtrlKeyWidth = kBaseKeyWidth * 1.5;
static constexpr float kSuperKeyWidth = kCtrlKeyWidth;
static constexpr float kAltKeyWidth = kCtrlKeyWidth;
static constexpr float kShiftKeyWidth = kBaseKeyWidth * 2.25;

static constexpr float kKeySpacing = kMargin;

static constexpr float kFrameWidth = kBorderWidth * 2 + kMargin;
static constexpr float kFrameInnerRadius = kKeyBaseRadius + kKeySpacing;
static constexpr float kFrameOuterRadius = kFrameInnerRadius + kFrameWidth;

static constexpr float kShortcutKeyWidth =
    kCtrlKeyWidth + kSuperKeyWidth + kAltKeyWidth - kShiftKeyWidth - kMinimalTouchableSize;

static constexpr float kBottomRowWidth = kFrameWidth + kKeySpacing + kCtrlKeyWidth + kKeySpacing +
                                         kSuperKeyWidth + kKeySpacing + kAltKeyWidth + kKeySpacing +
                                         kFrameWidth;
static constexpr float kTopRowWidth = kFrameWidth + kKeySpacing + kShiftKeyWidth + kKeySpacing +
                                      kShortcutKeyWidth + kKeySpacing + kFrameWidth;

static constexpr float kWidth = std::max(kTopRowWidth, kBottomRowWidth);
static constexpr float kHeight = kFrameWidth * 2 + kKeyHeight * 2 + kKeySpacing * 3;
static constexpr float kY = -kHeight / 2 - 0.5_mm;
static constexpr Rect kShapeRect = Rect::MakeCenterWH({0, 0.5_mm}, kWidth, kHeight);
static const SkRRect kShapeRRect = [] {
  SkRRect ret;
  float top_right_radius = kFrameWidth + kMinimalTouchableSize / 2 - kBorderWidth;
  SkVector radii[4] = {
      {kFrameOuterRadius, kFrameOuterRadius},
      {kFrameOuterRadius, kFrameOuterRadius},
      {top_right_radius, top_right_radius},
      {kFrameOuterRadius, kFrameOuterRadius},
  };
  ret.setRectRadii(kShapeRect.sk, radii);
  return ret;
}();

static SkPaint& GetFirePaint(const Rect& rect, float radius) {
  static SkRuntimeShaderBuilder builder = []() {
    const char* sksl = R"( // Fire shader
vec2 hash(vec2 p) {
	p = vec2( dot(p,vec2(127.1,311.7)),
			 dot(p,vec2(269.5,183.3)) );
	return -1.0 + 2.0*fract(sin(p)*43758.5453123);
}

float noise(in vec2 p) {
	const float K1 = 0.366025404; // (sqrt(3)-1)/2;
	const float K2 = 0.211324865; // (3-sqrt(3))/6;
	vec2 i = floor( p + (p.x+p.y)*K1 );
	vec2 a = p - i + (i.x+i.y)*K2;
	vec2 o = (a.x>a.y) ? vec2(1.0,0.0) : vec2(0.0,1.0);
	vec2 b = a - o + K2;
	vec2 c = a - 1.0 + 2.0*K2;
	vec3 h = max( 0.5-vec3(dot(a,a), dot(b,b), dot(c,c) ), 0.0 );
	vec3 n = h*h*h*h*vec3( dot(a,hash(i+0.0)), dot(b,hash(i+o)), dot(c,hash(i+1.0)));
	return dot( n, vec3(70.0) );
}

float fbm(vec2 uv) {
	float f;
	mat2 m = mat2( 1.6,  1.2, -1.2,  1.6 );
	f  = 0.5000*noise( uv ); uv = m*uv;
	f += 0.2500*noise( uv ); uv = m*uv;
	f += 0.1250*noise( uv ); uv = m*uv;
	f += 0.0625*noise( uv ); uv = m*uv;
	f += 0.0625*noise( uv ); uv = m*uv;
	f = 0.5 + 0.5*f;

	return f;
}

uniform float iTime;
uniform float iLeft;
uniform float iRight;
uniform float iTop;
uniform float iBottom;
uniform float iDetail;
uniform float iSmokeDetail;
uniform float iRadius;

vec4 main(in vec2 fragCoord) {
	vec2 uv = (fragCoord - vec2(iLeft, iBottom)) / vec2(iRight - iLeft, iTop - iBottom);
	float n = fbm(iDetail * fragCoord - vec2(0,iTime));
  //return vec4(n, n, n, 1.0);
  vec2 d = max(vec2(0, 0), vec2(max(iLeft - fragCoord.x, fragCoord.x - iRight), max(iBottom - fragCoord.y, fragCoord.y - iTop))) / iRadius;
  float l = length(d);
  //return vec4(l, l, l, 1.0);
	float c = 4 * (n * max(0.5, uv.y) - l);
  c = clamp(c, 0, 1);
  // return vec4(c, c, c, 1.0);
	float c1 = n * c;
  // return vec4(c1, c1, c1, 1.0);
	return vec4(1.5*c1, 1.5*c1*c1*c1, c1*c1*c1*c1*c1*c1, 1) * c;
})";

    auto [effect, err] = SkRuntimeEffect::MakeForShader(SkString(sksl));
    if (!err.isEmpty()) {
      FATAL << err.c_str();
    }
    return SkRuntimeShaderBuilder(effect);
  }();

  static auto start = std::chrono::steady_clock::now();
  float delta = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
  builder.uniform("iTime") = delta * 3;
  builder.uniform("iLeft") = rect.left;
  builder.uniform("iRight") = rect.right;
  builder.uniform("iTop") = rect.top;
  builder.uniform("iBottom") = rect.bottom;
  builder.uniform("iDetail") = 80.f;
  builder.uniform("iSmokeDetail") = 100.f;
  builder.uniform("iRadius") = radius;
  static SkPaint paint;
  paint.setShader(builder.makeShader());
  paint.setBlendMode(SkBlendMode::kHardLight);
  return paint;
}

// static void GrabEverything(HotKey& hotkey) {
//   for (auto keycode : x11::kKeyCodesArray) {
//     auto cookie =
//         xcb_grab_key_checked(connection, 0, screen->root, XCB_MOD_MASK_ANY,
//         (xcb_keycode_t)keycode,
//                              XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
//     if (auto err = xcb_request_check(connection, cookie)) {
//       LOG << "Failed to grab key: " << ToStr(x11::X11KeyCodeToKey(keycode)) << " (keycode "
//           << (int)keycode << "): " << err->error_code;
//     }
//   }
//   BeginIntercepting(hotkey);
// }

// static void UngrabEverything(HotKey& hotkey) {
//   auto cookie = xcb_ungrab_key_checked(connection, XCB_GRAB_ANY, screen->root, XCB_MOD_MASK_ANY);
//   if (auto err = xcb_request_check(connection, cookie)) {
//     FATAL << "Failed to ungrab key: " << err->error_code;
//   }
// }

HotKey::HotKey() {
  power_button = std::make_shared<PowerButton>(this);
  ctrl_button =
      std::make_shared<KeyButton>(MakeKeyLabelWidget("Ctrl"), KeyColor(ctrl), kCtrlKeyWidth);
  alt_button = std::make_shared<KeyButton>(MakeKeyLabelWidget("Alt"), KeyColor(alt), kAltKeyWidth);
  shift_button =
      std::make_shared<KeyButton>(MakeKeyLabelWidget("Shift"), KeyColor(shift), kShiftKeyWidth);
  windows_button =
      std::make_shared<KeyButton>(MakeKeyLabelWidget("Super"), KeyColor(windows), kSuperKeyWidth);
  shortcut_button =
      std::make_shared<KeyButton>(MakeKeyLabelWidget("?"), KeyColor(true), kShortcutKeyWidth);

  power_button->local_to_parent =
      SkM44::Translate(kWidth / 2 - kFrameWidth - kMinimalTouchableSize + kBorderWidth,
                       kShapeRect.top - kFrameWidth - kMinimalTouchableSize + kBorderWidth);

  ctrl_button->local_to_parent = SkM44::Translate(-kWidth / 2 + kFrameWidth + kKeySpacing,
                                                  kShapeRect.bottom + kFrameWidth + kKeySpacing);

  windows_button->local_to_parent =
      SkM44::Translate(-kWidth / 2 + kFrameWidth + kKeySpacing * 2 + kCtrlKeyWidth,
                       kShapeRect.bottom + kFrameWidth + kKeySpacing);

  alt_button->local_to_parent =
      SkM44::Translate(-kWidth / 2 + kFrameWidth + kKeySpacing * 3 + kCtrlKeyWidth + kSuperKeyWidth,
                       kShapeRect.bottom + kFrameWidth + kKeySpacing);

  shift_button->local_to_parent =
      SkM44::Translate(-kWidth / 2 + kFrameWidth + kKeySpacing,
                       kShapeRect.bottom + kFrameWidth + kKeySpacing * 2 + kKeyHeight);

  shortcut_button->local_to_parent =
      SkM44::Translate(-kWidth / 2 + kFrameWidth + kKeySpacing * 2 + kShiftKeyWidth,
                       kShapeRect.bottom + kFrameWidth + kKeySpacing * 2 + kKeyHeight);

  ctrl_button->activate = [this](gui::Pointer&) {
    bool on = IsOn();
    if (on) {
      Off();  // temporarily switch off to ungrab the old key combo
    }
    ctrl = !ctrl;
    if (on) {
      On();
    }
    ctrl_button->fg = KeyColor(ctrl);
  };
  alt_button->activate = [this](gui::Pointer&) {
    bool on = IsOn();
    if (on) {
      Off();  // temporarily switch off to ungrab the old key combo
    }
    alt = !alt;
    if (on) {
      On();
    }
    alt_button->fg = KeyColor(alt);
  };
  shift_button->activate = [this](gui::Pointer&) {
    bool on = IsOn();
    if (on) {
      Off();  // temporarily switch off to ungrab the old key combo
    }
    shift = !shift;
    if (on) {
      On();
    }
    shift_button->fg = KeyColor(shift);
  };
  windows_button->activate = [this](gui::Pointer&) {
    bool on = IsOn();
    if (on) {
      Off();  // temporarily switch off to ungrab the old key combo
    }
    windows = !windows;
    if (on) {
      On();
    }
    windows_button->fg = KeyColor(windows);
  };
  shortcut_button->SetLabel(ToStr(key));
  shortcut_button->activate = [this](gui::Pointer& pointer) {
    if (hotkey_selector) {
      // Cancel HotKey selection.
      hotkey_selector->Release();  // This will also set itself to nullptr
    } else {
      hotkey_selector = &pointer.keyboard->RequestGrab(*this);
      shortcut_button->WakeAnimation();
    }
  };
}
string_view HotKey::Name() const { return "HotKey"; }
std::shared_ptr<Object> HotKey::Clone() const {
  auto ret = std::make_shared<HotKey>();
  ret->key = key;
  ret->ctrl = ctrl;
  ret->alt = alt;
  ret->shift = shift;
  ret->windows = windows;
  return ret;
}

animation::Phase HotKey::Tick(time::Timer& t) {
  if (hotkey_selector) {
    shortcut_button->fg = kKeyGrabbingColor;
  } else {
    shortcut_button->fg = KeyColor(true);
  }
  return animation::Finished;
}

void HotKey::Draw(SkCanvas& canvas) const {
  SkRRect frame_outer;
  SkRRect frame_inner;
  SkRRect frame_inner2;
  kShapeRRect.inset(kBorderWidth, kBorderWidth, &frame_outer);
  frame_outer.inset(kMargin, kMargin, &frame_inner);
  frame_inner.inset(kBorderWidth, kBorderWidth, &frame_inner2);

  float start_x = frame_inner2.rect().right();
  float start_y = frame_inner2.rect().top() + kFrameInnerRadius;
  ArcLine inner_outline = ArcLine({start_x, start_y}, 90_deg);
  inner_outline.MoveBy(kKeySpacing + kKeyHeight - kKeyBaseRadius - kFrameInnerRadius);
  inner_outline.TurnConvex(90_deg, kFrameInnerRadius);
  inner_outline.MoveBy(kMinimalTouchableSize / 2 - kFrameInnerRadius);
  inner_outline.TurnConvex(-90_deg, kMinimalTouchableSize / 2 + kKeySpacing);
  inner_outline.MoveBy(kMinimalTouchableSize / 2 - kFrameInnerRadius);
  inner_outline.TurnConvex(90_deg, kFrameInnerRadius);
  inner_outline.MoveBy(frame_inner2.width() - kFrameInnerRadius * 2 - kMinimalTouchableSize -
                       kKeySpacing);
  inner_outline.TurnConvex(90_deg, kFrameInnerRadius);
  inner_outline.MoveBy(frame_inner2.height() - kFrameInnerRadius * 2);
  inner_outline.TurnConvex(90_deg, kFrameInnerRadius);
  inner_outline.MoveBy(frame_inner2.width() - kFrameInnerRadius * 2);
  inner_outline.TurnConvex(90_deg, kFrameInnerRadius);
  SkPath inner_contour = inner_outline.ToPath();

  // Draw background
  SkPaint inner_paint;
  inner_paint.setColor("#000000"_color);
  inner_paint.setStyle(SkPaint::kStrokeAndFill_Style);
  inner_paint.setStrokeWidth(0.5_mm);
  canvas.drawPath(inner_contour, inner_paint);

  // Rect inner_rect = Rect{
  //     -kWidth / 2 + kFrameWidth,
  //     -kHeight / 2 + kFrameWidth,
  //     kWidth / 2 - kFrameWidth,
  //     kHeight / 2 - kFrameWidth,
  // };
  // float fire_radius = 10_mm;
  // auto fire_paint = GetFirePaint(inner_rect, fire_radius);
  // inner_rect.sk.outset(fire_radius, fire_radius * 1.5);

  // canvas.drawRect(inner_rect.sk, fire_paint);

  // Frame shadow
  SkPaint background_shadow_paint;
  background_shadow_paint.setMaskFilter(
      SkMaskFilter::MakeBlur(SkBlurStyle::kInner_SkBlurStyle, 0.0005, true));
  background_shadow_paint.setColor("#333333"_color);
  canvas.drawPath(inner_contour, background_shadow_paint);

  // Draw frame

  SkRect gradient_rect = kShapeRect.sk.makeInset(kFrameWidth / 2, kFrameWidth / 2);
  float gradient_r = kFrameInnerRadius;

  SkPaint border_paint;
  border_paint.setAntiAlias(true);
  SkPoint border_pts[] = {{0, kShapeRect.top}, {0, kShapeRect.bottom}};
  SkColor border_colors[] = {"#f0f0f0"_color, "#cccccc"_color};
  border_paint.setShader(
      SkGradientShader::MakeLinear(border_pts, border_colors, nullptr, 2, SkTileMode::kClamp));

  SkPath border_path;
  border_path.addRRect(kShapeRRect);
  border_path.addPath(inner_contour);
  border_path.setFillType(SkPathFillType::kEvenOdd);
  canvas.drawPath(border_path, border_paint);

  SkBlendMode shade_blend_mode = SkBlendMode::kHardLight;
  float shade_alpha = 0.5;
  SkPoint light_pts[] = {{0, kShapeRect.top}, {0, kShapeRect.bottom}};
  SkColor light_colors[] = {"#fdf8e0"_color, "#111c22"_color};
  SkPaint light_paint;
  light_paint.setAntiAlias(true);
  light_paint.setBlendMode(shade_blend_mode);
  light_paint.setAlphaf(shade_alpha);
  light_paint.setShader(
      SkGradientShader::MakeLinear(light_pts, light_colors, nullptr, 2, SkTileMode::kClamp));

  canvas.drawDRRect(kShapeRRect, frame_outer, light_paint);

  SkPoint shadow_pts[] = {{0, kShapeRect.bottom + kFrameOuterRadius}, {0, kShapeRect.bottom}};
  SkColor shadow_colors[] = {"#111c22"_color, "#fdf8e0"_color};
  SkPaint shadow_paint;
  shadow_paint.setAntiAlias(true);
  shadow_paint.setBlendMode(shade_blend_mode);
  shadow_paint.setAlphaf(shade_alpha);
  shadow_paint.setStyle(SkPaint::kStroke_Style);
  shadow_paint.setStrokeWidth(kBorderWidth * 2);
  shadow_paint.setShader(
      SkGradientShader::MakeLinear(shadow_pts, shadow_colors, nullptr, 2, SkTileMode::kClamp));
  canvas.save();
  canvas.clipPath(border_path, true);
  canvas.drawPath(inner_contour, shadow_paint);
  canvas.restore();

  DrawChildren(canvas);
}

SkPath HotKey::Shape() const { return SkPath::RRect(kShapeRRect); }
void HotKey::Args(std::function<void(Argument&)> cb) { cb(next_arg); }

void HotKey::FillChildren(maf::Vec<std::shared_ptr<Widget>>& children) {
  children.push_back(power_button);
  children.push_back(ctrl_button);
  children.push_back(alt_button);
  children.push_back(shift_button);
  children.push_back(windows_button);
  children.push_back(shortcut_button);
}

bool HotKey::IsOn() const { return hotkey != nullptr; }

void HotKey::On() {
  if (hotkey) {  // just a sanity check, we should never get On multiple times in a row
    hotkey->Release();
  }
  hotkey =
      &gui::keyboard->RequestKeyGrab(*this, key, ctrl, alt, shift, windows, [&](Status& status) {
        if (!OK(status)) {
          if (hotkey) {
            hotkey->Release();
          }
          ERROR << status;
        }
      });
}

// This is called when the new HotKey is selected by the user
void HotKey::KeyboardGrabberKeyDown(gui::KeyboardGrab&, gui::Key key) {
  bool on = IsOn();
  if (on) {
    Off();  // temporarily switch off to ungrab the old key combo
  }
  hotkey_selector->Release();
  // Maybe also set the modifiers from the key event?
  this->key = key.physical;
  shortcut_button->SetLabel(ToStr(key.physical));
  if (on) {
    On();
  }
}

void HotKey::KeyGrabberKeyDown(gui::KeyGrab&) {
  if (auto h = here.lock()) {
    ScheduleNext(*h);
  }
}
void HotKey::KeyGrabberKeyUp(gui::KeyGrab&) {}

void HotKey::Off() {
  // TODO: think about what happens if the recording starts or stops while the hotkey is active &
  // vice versa
  if (hotkey) {
    hotkey->Release();
  }
}

void HotKey::ReleaseGrab(gui::KeyboardGrab&) {
  hotkey_selector = nullptr;
  LOG << "HotKey::ReleaseGrab - invalidating shortcut_button!";
  shortcut_button->WakeAnimation();
}
void HotKey::ReleaseKeyGrab(gui::KeyGrab&) { hotkey = nullptr; }

void HotKey::SerializeState(Serializer& writer, const char* key) const {
  writer.Key(key);
  writer.StartObject();
  writer.Key("key");
  auto key_str = ToStr(this->key);
  writer.String(key_str.data(), key_str.size(), true);
  writer.Key("ctrl");
  writer.Bool(ctrl);
  writer.Key("alt");
  writer.Bool(alt);
  writer.Key("shift");
  writer.Bool(shift);
  writer.Key("windows");
  writer.Bool(windows);
  writer.Key("active");
  writer.Bool(IsOn());
  writer.EndObject();
}

void HotKey::DeserializeState(Location& l, Deserializer& d) {
  Status status;
  bool on = IsOn();
  if (on) {
    Off();  // temporarily switch off to ungrab the old key combo
  }
  for (auto& key : ObjectView(d, status)) {
    if (key == "key") {
      Str key_str;
      d.Get(key_str, status);
      if (OK(status)) {
        this->key = AnsiKeyFromStr(key_str);
      }
    } else if (key == "ctrl") {
      d.Get(ctrl, status);
    } else if (key == "alt") {
      d.Get(alt, status);
    } else if (key == "shift") {
      d.Get(shift, status);
    } else if (key == "windows") {
      d.Get(windows, status);
    } else if (key == "active") {
      d.Get(on, status);
    }
  }
  if (on) {
    On();
  }

  shortcut_button->SetLabel(ToStr(this->key));
  ctrl_button->fg = KeyColor(ctrl);
  alt_button->fg = KeyColor(alt);
  shift_button->fg = KeyColor(shift);
  windows_button->fg = KeyColor(windows);

  if (!OK(status)) {
    l.ReportError(status.ToStr());
  }
}

}  // namespace automat::library