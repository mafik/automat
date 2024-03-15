#include "library_hotkey.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkShader.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>

#include <chrono>
#include <cmath>
#include <memory>

#include "arcline.hh"
#include "color.hh"
#include "font.hh"
#include "gui_constants.hh"
#include "gui_shape_widget.hh"
#include "library_macros.hh"
#include "math.hh"
#include "svg.hh"
#include "text_field.hh"

#if defined(__linux__)
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>

#include "linux_main.hh"
#include "x11.hh"
#endif

using namespace automat::gui;

namespace automat::library {

DEFINE_PROTO(HotKey);

static constexpr float kKeyLetterSize = 2.4_mm;
static constexpr float kKeyLetterSizeMM = kKeyLetterSize * 1000;

static Font& KeyFont() {
  static std::unique_ptr<Font> font = Font::Make(kKeyLetterSizeMM, 700);
  return *font.get();
}

static constexpr float kKeyHeight = kMinimalTouchableSize;
static constexpr float kKeySpareHeight = kKeyHeight - kKeyLetterSize;
static constexpr float kKeyTopSide = 0.5_mm;
static constexpr float kKeyBottomSide = 1.5_mm;
static constexpr float kKeyMargin = (kKeyHeight - kKeyTopSide - kKeyBottomSide) / 2;
static constexpr float kKeySide = 1_mm;

static constexpr float kKeyFaceRadius = 1_mm;
static constexpr float kKeyBaseRadius = kKeyFaceRadius;
static constexpr float kKeyFaceHeight = kKeyHeight - kKeyTopSide - kKeyBottomSide;

static constexpr float kBaseKeyWidth = kKeyHeight;
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

static constexpr SkColor kKeyEnabledColor = "#f3a75b"_color;
static constexpr SkColor kKeyDisabledColor = "#f4efea"_color;

static constexpr SkRect kShapeRect = SkRect::MakeXYWH(-kWidth / 2, -kHeight / 2, kWidth, kHeight);
static const SkRRect kShapeRRect = [] {
  SkRRect ret;
  float top_right_radius = kFrameWidth + kMinimalTouchableSize / 2 - kBorderWidth;
  SkVector radii[4] = {
      {kFrameOuterRadius, kFrameOuterRadius},
      {kFrameOuterRadius, kFrameOuterRadius},
      {top_right_radius, top_right_radius},
      {kFrameOuterRadius, kFrameOuterRadius},
  };
  ret.setRectRadii(kShapeRect, radii);
  return ret;
}();

struct KeyLabelWidget : Widget {
  Str label;
  float width;

  KeyLabelWidget(StrView label) { SetLabel(label); }
  SkPath Shape() const override { return SkPath::Rect(SkRect::MakeWH(width, kKeyLetterSize)); }
  void Draw(DrawContext& ctx) const override {
    SkPaint paint;
    paint.setAntiAlias(true);
    paint.setColor("#000000"_color);
    ctx.canvas.translate(-width / 2, -kKeyLetterSize / 2);
    KeyFont().DrawText(ctx.canvas, label, paint);
    ctx.canvas.translate(width / 2, kKeyLetterSize / 2);
  }
  void SetLabel(StrView label) {
    this->label = label;
    width = KeyFont().MeasureText(label);
  }
};

std::unique_ptr<Widget> MakeKeyLabelWidget(StrView label) {
  return std::make_unique<KeyLabelWidget>(label);
}

KeyButton::KeyButton(std::unique_ptr<Widget> child, SkColor color, float width)
    : Button(std::move(child), color), width(width) {}

void KeyButton::Activate(gui::Pointer& pointer) {
  if (activate) {
    activate(pointer);
  }
}

SkRRect KeyButton::RRect() const {
  return SkRRect::MakeRectXY(SkRect::MakeWH(width, kKeyHeight), kKeyBaseRadius, kKeyBaseRadius);
}

static sk_sp<SkShader> MakeSweepShader(const RRect& rrect, SkColor side_color, SkColor top_color,
                                       SkColor top_corner_top, SkColor top_corner_side,
                                       SkColor bottom_corner_side, SkColor bottom_corner_bottom,
                                       SkColor bottom_color) {
  SkColor colors[] = {
      side_color,            // right middle
      top_corner_side,       // bottom of top-right corner
      top_corner_top,        // top of the top-right corner
      top_color,             // center top
      top_corner_top,        // top of the top-left corner
      top_corner_side,       // bottom of the top-left corner
      side_color,            // left middle
      bottom_corner_side,    // top of the bottom-left corner
      bottom_corner_bottom,  // bottom of the bottom-left corner
      bottom_color,          // center bottom
      bottom_corner_bottom,  // bottom of the bottom-right corner
      bottom_corner_side,    // top of the bottom-right corner
      side_color,            // right middle
  };
  auto center = rrect.Center();
  float pos[] = {0,
                 (float)(atan(rrect.LineEndRightUpper() - center) / (2 * M_PI)),
                 (float)(atan(rrect.LineEndUpperRight() - center) / (2 * M_PI)),
                 0.25,
                 (float)(atan(rrect.LineEndUpperLeft() - center) / (2 * M_PI)),
                 (float)(atan(rrect.LineEndLeftUpper() - center) / (2 * M_PI)),
                 0.5,
                 (float)(atan(rrect.LineEndLeftLower() - center) / (2 * M_PI) + 1),
                 (float)(atan(rrect.LineEndLowerLeft() - center) / (2 * M_PI) + 1),
                 0.75,
                 (float)(atan(rrect.LineEndLowerRight() - center) / (2 * M_PI) + 1),
                 (float)(atan(rrect.LineEndRightLower() - center) / (2 * M_PI) + 1),
                 1};
  return SkGradientShader::MakeSweep(center.x, center.y, colors, pos, 13);
}

void KeyButton::DrawButtonFace(gui::DrawContext& ctx, SkColor bg, SkColor fg) const {
  auto& canvas = ctx.canvas;
  auto& actx = ctx.animation_context;
  auto& press = press_ptr[actx];
  auto& hover = hover_ptr[actx];
  bool enabled = false;

  SkRRect key_base = RRect();
  float press_shift_y = press * -kPressOffset;
  key_base.offset(0, press_shift_y);

  SkRRect key_face = SkRRect::MakeRectXY(
      SkRect::MakeLTRB(key_base.rect().left() + kKeySide, key_base.rect().top() + kKeyBottomSide,
                       key_base.rect().right() - kKeySide, key_base.rect().bottom() - kKeyTopSide),
      kKeyFaceRadius, kKeyFaceRadius);

  float lightness_adjust = hover * 10;

  SkPaint face_paint;
  SkPoint face_pts[] = {{0, key_face.rect().bottom()}, {0, key_face.rect().top()}};
  SkColor face_colors[] = {color::AdjustLightness(color, -10 + lightness_adjust),
                           color::AdjustLightness(color, lightness_adjust)};
  face_paint.setShader(
      SkGradientShader::MakeLinear(face_pts, face_colors, nullptr, 2, SkTileMode::kClamp));

  face_paint.setStyle(SkPaint::kStrokeAndFill_Style);
  face_paint.setStrokeWidth(0.5_mm);

  canvas.drawRRect(key_face, face_paint);

  SkColor top_color = color::AdjustLightness(color, 20 + lightness_adjust);
  SkColor side_color = color::AdjustLightness(color, -20 + lightness_adjust);
  SkColor side_color2 = color::AdjustLightness(color, -25 + lightness_adjust);
  SkColor bottom_color = color::AdjustLightness(color, -50 + lightness_adjust);

  SkPaint side_paint;
  side_paint.setAntiAlias(true);
  side_paint.setShader(MakeSweepShader(*reinterpret_cast<union RRect*>(&key_face), side_color,
                                       top_color, top_color, side_color, side_color2, bottom_color,
                                       bottom_color));
  canvas.drawDRRect(key_base, key_face, side_paint);

  if (auto paint = PaintMixin::Get(child.get())) {
    paint->setColor(fg);
    paint->setAntiAlias(true);
  }
  canvas.translate(key_face.rect().centerX(), key_face.rect().centerY());
  child->Draw(ctx);
  canvas.translate(-key_face.rect().centerX(), -key_face.rect().centerY());
}

PowerButton::PowerButton(OnOff* target)
    : ToggleButton(MakeShapeWidget(kPowerSVG, SK_ColorWHITE), "#fa2305"_color), target(target) {}

static SkColor KeyColor(bool enabled) { return enabled ? kKeyEnabledColor : kKeyDisabledColor; }

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
    SkRuntimeShaderBuilder builder(effect);
    return builder;
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

HotKey::HotKey()
    : power_button(this),
      ctrl_button(MakeKeyLabelWidget("Ctrl"), KeyColor(ctrl), kCtrlKeyWidth),
      alt_button(MakeKeyLabelWidget("Alt"), KeyColor(alt), kAltKeyWidth),
      shift_button(MakeKeyLabelWidget("Shift"), KeyColor(shift), kShiftKeyWidth),
      windows_button(MakeKeyLabelWidget("Super"), KeyColor(windows), kSuperKeyWidth),
      shortcut_button(MakeKeyLabelWidget("?"), KeyColor(true), kShortcutKeyWidth) {
  ctrl_button.activate = [this](gui::Pointer&) {
    if (on) {
      Off();  // temporarily switch off to ungrab the old key combo
    }
    ctrl = !ctrl;
    if (on) {
      On();
    }
    ctrl_button.color = KeyColor(ctrl);
  };
  alt_button.activate = [this](gui::Pointer&) {
    if (on) {
      Off();  // temporarily switch off to ungrab the old key combo
    }
    alt = !alt;
    if (on) {
      On();
    }
    alt_button.color = KeyColor(alt);
  };
  shift_button.activate = [this](gui::Pointer&) {
    if (on) {
      Off();  // temporarily switch off to ungrab the old key combo
    }
    shift = !shift;
    if (on) {
      On();
    }
    shift_button.color = KeyColor(shift);
  };
  windows_button.activate = [this](gui::Pointer&) {
    if (on) {
      Off();  // temporarily switch off to ungrab the old key combo
    }
    windows = !windows;
    if (on) {
      On();
    }
    windows_button.color = KeyColor(windows);
  };
  ((KeyLabelWidget*)shortcut_button.child.get())->SetLabel(ToStr(key));
  shortcut_button.activate = [this](gui::Pointer& pointer) {
    if (recording) {
      recording->Release();
    } else {
      recording = &pointer.keyboard->RequestGrab(*this);
    }
  };
}
string_view HotKey::Name() const { return "HotKey"; }
std::unique_ptr<Object> HotKey::Clone() const {
  auto ret = std::make_unique<HotKey>();
  ret->key = key;
  ret->ctrl = ctrl;
  ret->alt = alt;
  ret->shift = shift;
  ret->windows = windows;
  return ret;
}

static void DrawCenteredText(SkCanvas& canvas, const char* text) {
  SkPaint text_paint;
  text_paint.setAntiAlias(true);
  text_paint.setColor("#000000"_color);
  static auto& font = KeyFont();
  float w = font.MeasureText(text);
  canvas.translate(-w / 2, -kKeyLetterSize / 2);
  font.DrawText(canvas, text, text_paint);
  canvas.translate(w / 2, kKeyLetterSize / 2);
}

void HotKey::Draw(gui::DrawContext& ctx) const {
  auto& canvas = ctx.canvas;

  SkRRect frame_outer;
  SkRRect frame_inner;
  SkRRect frame_inner2;
  kShapeRRect.inset(kBorderWidth, kBorderWidth, &frame_outer);
  frame_outer.inset(kMargin, kMargin, &frame_inner);
  frame_inner.inset(kBorderWidth, kBorderWidth, &frame_inner2);

  float start_x = frame_inner2.rect().right();
  float start_y = frame_inner2.rect().top() + kFrameInnerRadius;
  ArcLine inner_outline = ArcLine({start_x, start_y}, M_PI / 2);
  inner_outline.MoveBy(kKeySpacing + kKeyHeight - kKeyBaseRadius - kFrameInnerRadius);
  inner_outline.TurnBy(M_PI / 2, kFrameInnerRadius);
  inner_outline.MoveBy(kMinimalTouchableSize / 2 - kFrameInnerRadius);
  inner_outline.TurnBy(-M_PI / 2, kMinimalTouchableSize / 2 + kKeySpacing);
  inner_outline.MoveBy(kMinimalTouchableSize / 2 - kFrameInnerRadius);
  inner_outline.TurnBy(M_PI / 2, kFrameInnerRadius);
  inner_outline.MoveBy(frame_inner2.width() - kFrameInnerRadius * 2 - kMinimalTouchableSize -
                       kKeySpacing);
  inner_outline.TurnBy(M_PI / 2, kFrameInnerRadius);
  inner_outline.MoveBy(frame_inner2.height() - kFrameInnerRadius * 2);
  inner_outline.TurnBy(M_PI / 2, kFrameInnerRadius);
  inner_outline.MoveBy(frame_inner2.width() - kFrameInnerRadius * 2);
  inner_outline.TurnBy(M_PI / 2, kFrameInnerRadius);
  SkPath inner_contour = inner_outline.ToPath();

  // Draw background
  SkPaint inner_paint;
  inner_paint.setColor("#000000"_color);
  inner_paint.setStyle(SkPaint::kStrokeAndFill_Style);
  inner_paint.setStrokeWidth(0.5_mm);
  canvas.drawPath(inner_contour, inner_paint);

  Rect inner_rect = {
      .left = -kWidth / 2 + kFrameWidth,
      .bottom = -kHeight / 2 + kFrameWidth,
      .right = kWidth / 2 - kFrameWidth,
      .top = kHeight / 2 - kFrameWidth,
  };
  float fire_radius = 10_mm;
  auto fire_paint = GetFirePaint(inner_rect, fire_radius);
  inner_rect.sk.outset(fire_radius, fire_radius * 1.5);

  canvas.drawRect(inner_rect.sk, fire_paint);

  // Frame shadow
  SkPaint background_shadow_paint;
  background_shadow_paint.setMaskFilter(
      SkMaskFilter::MakeBlur(SkBlurStyle::kInner_SkBlurStyle, 0.0005, true));
  background_shadow_paint.setColor("#333333"_color);
  canvas.drawPath(inner_contour, background_shadow_paint);

  // Draw frame

  SkRect gradient_rect = kShapeRect.makeInset(kFrameWidth / 2, kFrameWidth / 2);
  float gradient_r = kFrameInnerRadius;

  SkPaint border_paint;
  border_paint.setAntiAlias(true);
  SkPoint border_pts[] = {{0, kShapeRect.bottom()}, {0, kShapeRect.top()}};
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
  SkPoint light_pts[] = {{0, kShapeRect.bottom()}, {0, kShapeRect.top()}};
  SkColor light_colors[] = {"#fdf8e0"_color, "#111c22"_color};
  SkPaint light_paint;
  light_paint.setAntiAlias(true);
  light_paint.setBlendMode(shade_blend_mode);
  light_paint.setAlphaf(shade_alpha);
  light_paint.setShader(
      SkGradientShader::MakeLinear(light_pts, light_colors, nullptr, 2, SkTileMode::kClamp));

  canvas.drawDRRect(kShapeRRect, frame_outer, light_paint);

  SkPoint shadow_pts[] = {{0, kShapeRect.top() + kFrameOuterRadius}, {0, kShapeRect.top()}};
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

  if (recording) {
    shortcut_button.color = "#f15555"_color;
  } else {
    shortcut_button.color = KeyColor(true);
  }

  DrawChildren(ctx);
}

SkPath HotKey::Shape() const { return SkPath::RRect(kShapeRRect); }
std::unique_ptr<Action> HotKey::ButtonDownAction(gui::Pointer&, gui::PointerButton) {
  return nullptr;
}
void HotKey::Args(std::function<void(Argument&)> cb) {}
void HotKey::Run(Location&) {}

ControlFlow HotKey::VisitChildren(gui::Visitor& visitor) {
  if (visitor(power_button) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  if (visitor(ctrl_button) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  if (visitor(alt_button) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  if (visitor(shift_button) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  if (visitor(windows_button) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  if (visitor(shortcut_button) == ControlFlow::Stop) {
    return ControlFlow::Stop;
  }
  return ControlFlow::Continue;
}

SkMatrix HotKey::TransformToChild(const Widget& child, animation::Context&) const {
  if (&child == &power_button) {
    return SkMatrix::Translate(-kWidth / 2 + kFrameWidth + kMinimalTouchableSize - kBorderWidth,
                               -kHeight / 2 + kFrameWidth + kMinimalTouchableSize - kBorderWidth);
  }
  if (&child == &ctrl_button) {
    return SkMatrix::Translate(kWidth / 2 - kFrameWidth - kKeySpacing,
                               kHeight / 2 - kFrameWidth - kKeySpacing);
  }
  if (&child == &windows_button) {
    return SkMatrix::Translate(kWidth / 2 - kFrameWidth - kKeySpacing * 2 - kCtrlKeyWidth,
                               kHeight / 2 - kFrameWidth - kKeySpacing);
  }
  if (&child == &alt_button) {
    return SkMatrix::Translate(
        kWidth / 2 - kFrameWidth - kKeySpacing * 3 - kCtrlKeyWidth - kSuperKeyWidth,
        kHeight / 2 - kFrameWidth - kKeySpacing);
  }
  if (&child == &shift_button) {
    return SkMatrix::Translate(kWidth / 2 - kFrameWidth - kKeySpacing,
                               kHeight / 2 - kFrameWidth - kKeySpacing * 2 - kKeyHeight);
  }
  if (&child == &shortcut_button) {
    return SkMatrix::Translate(kWidth / 2 - kFrameWidth - kKeySpacing * 2 - kShiftKeyWidth,
                               kHeight / 2 - kFrameWidth - kKeySpacing * 2 - kKeyHeight);
  }
  return SkMatrix::I();
}

void HotKey::On() {
  if (hotkey) {  // just a sanity check, we should never get On multiple times in a row
    hotkey->Release();
  }
  hotkey = &gui::keyboard->RequestKeyGrab(*this, key, ctrl, alt, shift, windows);
  // U16 modifiers = 0;
  // if (ctrl) {
  //   modifiers |= XCB_MOD_MASK_CONTROL;
  // }
  // if (alt) {
  //   modifiers |= XCB_MOD_MASK_1;
  // }
  // if (shift) {
  //   modifiers |= XCB_MOD_MASK_SHIFT;
  // }
  // if (windows) {
  //   modifiers |= XCB_MOD_MASK_4;
  // }
  // xcb_keycode_t keycode = (U8)x11::KeyToX11KeyCode(key);

  // for (bool caps_lock : {true, false}) {
  //   for (bool num_lock : {true, false}) {
  //     for (bool scroll_lock : {true, false}) {
  //       for (bool level3shift : {true, false}) {
  //         modifiers =
  //             caps_lock ? (modifiers | XCB_MOD_MASK_LOCK) : (modifiers & ~XCB_MOD_MASK_LOCK);
  //         modifiers = num_lock ? (modifiers | XCB_MOD_MASK_2) : (modifiers & ~XCB_MOD_MASK_2);
  //         modifiers = scroll_lock ? (modifiers | XCB_MOD_MASK_5) : (modifiers & ~XCB_MOD_MASK_5);
  //         modifiers = level3shift ? (modifiers | XCB_MOD_MASK_3) : (modifiers & ~XCB_MOD_MASK_3);
  //         auto cookie = xcb_grab_key(connection, 0, screen->root, modifiers, keycode,
  //                                    XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
  //         if (auto err = xcb_request_check(connection, cookie)) {
  //           FATAL << "Failed to grab key: " << err->error_code;
  //         }
  //       }
  //     }
  //   }
  // }
}

void HotKey::KeyboardGrabberKeyDown(gui::KeyboardGrab&, gui::Key key) {
  recording->Release();
  LOG << "Setting new hotkey " << (int)key.physical << ": " << ToStr(key.physical);
  ((KeyLabelWidget*)shortcut_button.child.get())->SetLabel(ToStr(key.physical));
  if (on) {
    On();
  }
}

void HotKey::KeyGrabberKeyDown(gui::KeyGrab&, gui::Key) { LOG << "Hotkey press"; }
void HotKey::KeyGrabberKeyUp(gui::KeyGrab&, gui::Key) { LOG << "Hotkey release"; }

void HotKey::Off() {
  // TODO: think about what happens if the recording starts or stops while the hotkey is active &
  // vice versa
  if (hotkey) {
    hotkey->Release();
  }

  // StopIntercepting(*this);
  // xcb_keycode_t keycode = (U8)x11::KeyToX11KeyCode(key);

  // auto cookie = xcb_ungrab_key_checked(connection, keycode, screen->root, XCB_MOD_MASK_ANY);
  // if (auto err = xcb_request_check(connection, cookie)) {
  //   FATAL << "Failed to ungrab key: " << err->error_code;
  // }
}

void HotKey::ReleaseGrab(gui::KeyboardGrab&) { recording = nullptr; }
void HotKey::ReleaseKeyGrab(gui::KeyGrab&) { hotkey = nullptr; }

}  // namespace automat::library