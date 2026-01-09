// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_hotkey.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkShader.h>
#include <include/effects/SkGradientShader.h>
#include <include/effects/SkRuntimeEffect.h>

#include "arcline.hh"
#include "color.hh"
#include "key_button.hh"
#include "keyboard.hh"
#include "math.hh"
#include "root_widget.hh"
#include "text_field.hh"
#include "ui_constants.hh"
#include "widget.hh"

#if defined(__linux__)
#include <xcb/xcb.h>
#include <xcb/xinput.h>
#include <xcb/xproto.h>
#endif

using namespace automat::ui;

namespace automat::library {

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
static constexpr Rect kShapeRect = Rect::MakeCenter({0, 0.5_mm}, kWidth, kHeight);
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

HotKey::HotKey(ui::Widget* parent)
    : WidgetBase(parent),
      power_button(new PowerButton(this, this)),
      ctrl_button(new KeyButton(this, "Ctrl", KeyColor(ctrl), kCtrlKeyWidth)),
      alt_button(new KeyButton(this, "Alt", KeyColor(alt), kAltKeyWidth)),
      shift_button(new KeyButton(this, "Shift", KeyColor(shift), kShiftKeyWidth)),
      windows_button(new KeyButton(this, "Super", KeyColor(windows), kSuperKeyWidth)),
      shortcut_button(new KeyButton(this, "?", KeyColor(true), kShortcutKeyWidth)) {
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

  ctrl_button->activate = [this](ui::Pointer&) {
    bool on = IsOn();
    if (on) {
      OnTurnOff();  // temporarily switch off to ungrab the old key combo
    }
    ctrl = !ctrl;
    if (on) {
      OnTurnOn();
    }
    ctrl_button->fg = KeyColor(ctrl);
  };
  alt_button->activate = [this](ui::Pointer&) {
    bool on = IsOn();
    if (on) {
      OnTurnOff();  // temporarily switch off to ungrab the old key combo
    }
    alt = !alt;
    if (on) {
      OnTurnOn();
    }
    alt_button->fg = KeyColor(alt);
  };
  shift_button->activate = [this](ui::Pointer&) {
    bool on = IsOn();
    if (on) {
      OnTurnOff();  // temporarily switch off to ungrab the old key combo
    }
    shift = !shift;
    if (on) {
      OnTurnOn();
    }
    shift_button->fg = KeyColor(shift);
  };
  windows_button->activate = [this](ui::Pointer&) {
    bool on = IsOn();
    if (on) {
      OnTurnOff();  // temporarily switch off to ungrab the old key combo
    }
    windows = !windows;
    if (on) {
      OnTurnOn();
    }
    windows_button->fg = KeyColor(windows);
  };
  shortcut_button->SetLabel(ToStr(key));
  shortcut_button->activate = [this](ui::Pointer& pointer) {
    if (hotkey_selector) {
      // Cancel HotKey selection.
      hotkey_selector->Release();  // This will also set itself to nullptr
    } else if (pointer.keyboard) {
      ui::Widget* label = shortcut_button->child.get();
      auto bounds = *label->TextureBounds();
      Vec2 caret_position = shortcut_button->RRect().rect().center();
      caret_position.x += bounds.left;
      hotkey_selector = &pointer.keyboard->RequestCaret(*this, this, caret_position);
    }
    WakeAnimation();
    shortcut_button->WakeAnimation();
  };
}
string_view HotKey::Name() const { return "HotKey"; }
Ptr<Object> HotKey::Clone() const {
  auto ret = MAKE_PTR(HotKey, parent);
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
void HotKey::Parts(const std::function<void(Part&)>& cb) { cb(next_arg); }

void HotKey::FillChildren(Vec<Widget*>& children) {
  children.push_back(power_button.get());
  children.push_back(ctrl_button.get());
  children.push_back(alt_button.get());
  children.push_back(shift_button.get());
  children.push_back(windows_button.get());
  children.push_back(shortcut_button.get());
}

bool HotKey::IsOn() const { return hotkey != nullptr; }

void HotKey::OnTurnOn() {
  if (hotkey) {  // just a sanity check, we should never get On multiple times in a row
    hotkey->Release();
  }
  auto& root_widget = FindRootWidget();
  hotkey = &root_widget.keyboard.RequestKeyGrab(*this, key, ctrl, alt, shift, windows,
                                                [&](Status& status) {
                                                  if (!OK(status)) {
                                                    if (hotkey) {
                                                      hotkey->Release();
                                                    }
                                                    ERROR << status;
                                                  }
                                                });
}

// This is called when the new HotKey is selected by the user
void HotKey::KeyDown(ui::Caret&, ui::Key key) {
  bool on = IsOn();
  if (on) {
    OnTurnOff();  // temporarily switch off to ungrab the old key combo
  }
  hotkey_selector->Release();
  // Maybe also set the modifiers from the key event?
  this->key = key.physical;
  shortcut_button->SetLabel(ToStr(key.physical));
  if (on) {
    OnTurnOn();
  }
}

void HotKey::KeyGrabberKeyDown(ui::KeyGrab&) { ScheduleNext(*this); }
void HotKey::KeyGrabberKeyUp(ui::KeyGrab&) {}

void HotKey::OnTurnOff() {
  // TODO: think about what happens if the recording starts or stops while the hotkey is active &
  // vice versa
  if (hotkey) {
    hotkey->Release();
  }
}

void HotKey::ReleaseCaret(ui::Caret&) {
  hotkey_selector = nullptr;
  WakeAnimation();
  shortcut_button->WakeAnimation();
}
void HotKey::ReleaseKeyGrab(ui::KeyGrab&) { hotkey = nullptr; }

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

void HotKey::DeserializeState(Deserializer& d) {
  Status status;
  bool on = IsOn();
  if (on) {
    OnTurnOff();  // temporarily switch off to ungrab the old key combo
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
    OnTurnOn();
  }

  shortcut_button->SetLabel(ToStr(this->key));
  ctrl_button->fg = KeyColor(ctrl);
  alt_button->fg = KeyColor(alt);
  shift_button->fg = KeyColor(shift);
  windows_button->fg = KeyColor(windows);

  if (!OK(status)) {
    ReportError(status.ToStr());
  }
}

}  // namespace automat::library
