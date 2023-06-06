#include "library_number.h"

#include <include/core/SkMatrix.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include <charconv>
#include <memory>

#include "color.h"
#include "drag_action.h"
#include "font.h"
#include "gui_align.h"
#include "gui_text.h"
#include "library_macros.h"
#include "widget.h"

namespace automat::library {

DEFINE_PROTO(Number);

// TODO: better icon for the backspace button
// TODO: LCD color for the text field
// TODO: Fix cursor location
// TODO: Adjust number text to right
// TODO: Remove underline from text display
// TODO: Buttons get highlighted immediately on mouse over
// TODO: When precision touchpad is pressed down, initial touch point is
// "forgotten" for the purpose of dragging
// TODO: "Iconified" drawing of widgets

static constexpr float kNumberInnerMargin = kMargin;
static constexpr float kNumberOuterMargin = kMargin;
static constexpr float kTextHeight =
    std::max(gui::kLetterSize + 2 * kNumberInnerMargin + 2 * kBorderWidth, kMinimalTouchableSize);
static constexpr float kButtonHeight = kMinimalTouchableSize;
static constexpr float kButtonRows = 4;
static constexpr float kRows = kButtonRows + 1;
static constexpr float kHeight = 2 * kBorderWidth + kTextHeight + kButtonRows * kButtonHeight +
                                 (kRows - 1) * kNumberInnerMargin + 2 * kNumberOuterMargin;

static constexpr float kButtonWidth = kMinimalTouchableSize;
static constexpr float kButtonColumns = 3;
static constexpr float kWidth = 2 * kBorderWidth + kButtonColumns * kButtonWidth +
                                (kButtonColumns - 1) * kNumberInnerMargin + 2 * kNumberOuterMargin;

static constexpr float kCornerRadius =
    kMinimalTouchableSize / 2 + kNumberOuterMargin + kBorderWidth;

using gui::AlignCenter;
using gui::Text;
using std::make_unique;

NumberButton::NumberButton(std::unique_ptr<Widget>&& child)
    : Button(make_unique<AlignCenter>(std::move(child))) {}

void NumberButton::Draw(gui::DrawContext& ctx) const { DrawButton(ctx, 0xffc8c4b7); }

void NumberButton::Activate() {
  if (activate) {
    activate();
  } else {
    LOG() << "NumberButton::Activate() called without callback";
  }
}

Number::Number(double x)
    : value(x),
      digits{NumberButton(make_unique<Text>("0")), NumberButton(make_unique<Text>("1")),
             NumberButton(make_unique<Text>("2")), NumberButton(make_unique<Text>("3")),
             NumberButton(make_unique<Text>("4")), NumberButton(make_unique<Text>("5")),
             NumberButton(make_unique<Text>("6")), NumberButton(make_unique<Text>("7")),
             NumberButton(make_unique<Text>("8")), NumberButton(make_unique<Text>("9"))},
      dot(make_unique<Text>(".")),
      backspace(make_unique<Text>("<")),
      text("0"),
      text_field(&text, kWidth - 2 * kNumberOuterMargin - 2 * kBorderWidth) {
  for (int i = 0; i < 10; ++i) {
    digits[i].activate = [this, i] {
      if (text.empty() || text == "0") {
        text = std::to_string(i);
      } else {
        text += std::to_string(i);
      }
      value = std::stod(text);
    };
  }
  dot.activate = [this] {
    if (text.empty()) {
      text = "0";
    } else if (auto it = text.find('.'); it != std::string::npos) {
      text.erase(it, 1);
    }
    text += ".";
    while (text.size() > 1 && text[0] == '0' && text[1] != '.') {
      text.erase(0, 1);
    }
    value = std::stod(text);
  };
  backspace.activate = [this] {
    if (!text.empty()) {
      text.pop_back();
    }
    if (text.empty()) {
      text = "0";
    }
    value = std::stod(text);
  };
}

string_view Number::Name() const { return "Number"; }

std::unique_ptr<Object> Number::Clone() const { return std::make_unique<Number>(value); }

string Number::GetText() const {
  char buffer[100];
  auto [end, ec] = std::to_chars(buffer, buffer + 100, value);
  *end = '\0';
  return buffer;
}

void Number::SetText(Location& error_context, string_view text) {
  value = std::stod(string(text));
  this->text = text;
}

static const SkRRect kNumberRRect = [] {
  SkRRect rrect;
  constexpr float kUpperRadius = gui::kTextCornerRadius + kNumberOuterMargin + kBorderWidth;
  SkVector radii[4] = {{kCornerRadius, kCornerRadius},
                       {kCornerRadius, kCornerRadius},
                       {kUpperRadius, kUpperRadius},
                       {kUpperRadius, kUpperRadius}};
  rrect.setRectRadii(SkRect::MakeXYWH(0, 0, kWidth, kHeight), radii);
  return rrect;
}();

static const SkRRect kNumberRRectInner = [] {
  SkRRect rrect;
  kNumberRRect.inset(kBorderWidth / 2, kBorderWidth / 2, &rrect);
  return rrect;
}();

static const SkPath kNumberShape = SkPath::RRect(kNumberRRect);

static const SkPaint kNumberBackgroundPaint = []() {
  SkPaint paint;
  SkPoint pts[2] = {{0, 0}, {0, kHeight}};
  SkColor colors[2] = {0xff483e37, 0xff6c5d53};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(gradient);
  return paint;
}();

static const SkPaint kNumberBorderPaint = []() {
  SkPaint paint_border;
  SkPoint pts[2] = {{0, 0}, {0, kHeight}};
  SkColor colors_border[2] = {0xff241f1c, 0xffac9d93};
  sk_sp<SkShader> gradient_border =
      SkGradientShader::MakeLinear(pts, colors_border, nullptr, 2, SkTileMode::kClamp);
  paint_border.setShader(gradient_border);
  paint_border.setAntiAlias(true);
  paint_border.setStyle(SkPaint::kStroke_Style);
  paint_border.setStrokeWidth(kBorderWidth);
  return paint_border;
}();

void Number::Draw(gui::DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  canvas.drawRRect(kNumberRRectInner, kNumberBackgroundPaint);
  canvas.drawRRect(kNumberRRectInner, kNumberBorderPaint);
  DrawChildren(ctx);
}

SkPath Number::Shape() const { return kNumberShape; }

gui::VisitResult Number::VisitChildren(gui::Visitor& visitor) {
  if (auto r = visitor(digits[0]); r != gui::VisitResult::kContinue) return r;
  if (auto r = visitor(dot); r != gui::VisitResult::kContinue) return r;
  if (auto r = visitor(backspace); r != gui::VisitResult::kContinue) return r;
  for (int i = 0; i < 10; ++i) {
    if (auto r = visitor(digits[i]); r != gui::VisitResult::kContinue) return r;
  }
  if (auto r = visitor(text_field); r != gui::VisitResult::kContinue) return r;
  return gui::VisitResult::kContinue;
}

SkMatrix Number::TransformToChild(const Widget* child, animation::Context*) const {
  auto cell = [](int row, int col) {
    float x = kBorderWidth + kNumberOuterMargin + col * (kButtonWidth + kNumberInnerMargin);
    float y = kBorderWidth + kNumberOuterMargin + row * (kButtonHeight + kNumberInnerMargin);
    return SkMatrix::Translate(-x, -y);
  };
  if (child == &text_field) {
    return cell(4, 0);
  }
  if (child == &digits[0]) {
    return cell(0, 0);
  }
  if (child == &dot) {
    return cell(0, 1);
  }
  if (child == &backspace) {
    return cell(0, 2);
  }
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      int digit = 3 * row + col + 1;
      if (child == &digits[digit]) {
        return cell(row + 1, col);
      }
    }
  }
  return SkMatrix::I();
}

std::unique_ptr<Action> Number::ButtonDownAction(gui::Pointer& pointer, gui::PointerButton btn) {
  if (btn != gui::PointerButton::kMouseLeft) {
    return nullptr;
  }
  auto& path = pointer.Path();
  if (path.size() < 2) {
    return nullptr;
  }
  auto* parent = path[path.size() - 2];
  Location* location = dynamic_cast<Location*>(parent);
  if (!location) {
    return nullptr;
  }
  std::unique_ptr<DragLocationAction> action = std::make_unique<DragLocationAction>(location);
  action->contact_point = pointer.PositionWithin(*this);
  LOG() << "Action contact point is " << action->contact_point;
  return action;
}

}  // namespace automat::library
