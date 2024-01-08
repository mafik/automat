#include "library_number.hh"

#include <include/core/SkMatrix.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include <charconv>
#include <memory>

#include "color.hh"
#include "control_flow.hh"
#include "drag_action.hh"
#include "font.hh"
#include "gui_align.hh"
#include "gui_shape_widget.hh"
#include "gui_text.hh"
#include "library_macros.hh"
#include "widget.hh"

namespace automat::library {

DEFINE_PROTO(Number);

// Margin used between rows and columns of buttons
static constexpr float kBetweenButtonsMargin = kMargin;

// Margin used around the entire widget
static constexpr float kAroundWidgetMargin = kMargin * 2;

static constexpr float kBelowTextMargin = kMargin * 2;

// Height of the text field
static constexpr float kTextHeight = std::max(
    gui::kLetterSize + 2 * kBetweenButtonsMargin + 2 * kBorderWidth, kMinimalTouchableSize);
static constexpr float kButtonHeight = kMinimalTouchableSize;
static constexpr float kButtonRows = 4;
static constexpr float kHeight = 2 * kBorderWidth + kTextHeight + kButtonRows * kButtonHeight +
                                 (kButtonRows - 1) * kBetweenButtonsMargin + kBelowTextMargin +
                                 2 * kAroundWidgetMargin;

static constexpr float kButtonWidth = kMinimalTouchableSize;
static constexpr float kButtonColumns = 3;
static constexpr float kWidth = 2 * kBorderWidth + kButtonColumns * kButtonWidth +
                                (kButtonColumns - 1) * kBetweenButtonsMargin +
                                2 * kAroundWidgetMargin;

static constexpr float kCornerRadius =
    kMinimalTouchableSize / 2 + kAroundWidgetMargin + kBorderWidth;
static constexpr char kBackspaceShape[] =
    "M-9 0-5.6 5.1A2 2 0 00-4 6H4A2 2 0 006 4V-4A2 2 0 004-6H-4A2 2 0 00-5.6-5.1ZM-3-4 0-1 3-4 4-3 "
    "1 0 4 3 3 4 0 1-3 4-4 3-1 0-4-3Z";

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
    LOG << "NumberButton::Activate() called without callback";
  }
}

NumberTextField::NumberTextField(Number& n)
    : gui::TextField(&n.text, kWidth - 2 * kAroundWidgetMargin - 2 * kBorderWidth) {}

SkRRect NumberTextField::ShapeRRect() const {
  return SkRRect::MakeRectXY(SkRect::MakeXYWH(0, 0, width, kTextHeight), kTextHeight / 2,
                             kTextHeight / 2);
}

static const SkPaint kNumberTextBackgroundPaint = []() {
  SkPoint pts[2] = {{0, 0}, {0, kTextHeight}};
  SkColor colors[2] = {0xffbec8b7, 0xffdee3db};
  sk_sp<SkShader> shader =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  SkPaint paint;
  paint.setShader(shader);
  return paint;
}();

const SkPaint& NumberTextField::GetBackgroundPaint() const { return kNumberTextBackgroundPaint; }

static const SkPaint kNumberTextBorderPaint = []() {
  SkPaint paint;
  SkPoint pts[2] = {{0, 0}, {0, kTextHeight}};
  SkColor colors[2] = {0xff917c6f, 0xff483e37};
  sk_sp<SkShader> shader =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(shader);
  paint.setAntiAlias(true);
  paint.setStyle(SkPaint::kStroke_Style);
  paint.setStrokeWidth(kBorderWidth);
  return paint;
}();

void NumberTextField::DrawBackground(gui::DrawContext& ctx) const {
  auto& canvas = ctx.canvas;
  SkRRect rrect = ShapeRRect();
  canvas.drawRRect(rrect, GetBackgroundPaint());
  canvas.drawRRect(rrect, kNumberTextBorderPaint);
}

void NumberTextField::DrawText(gui::DrawContext& ctx) const {
  gui::Font& font = gui::GetFont();
  Vec2 text_pos = GetTextPos();
  ctx.canvas.translate(text_pos.x, text_pos.y);
  font.DrawText(ctx.canvas, *text, GetTextPaint());
}

Vec2 NumberTextField::GetTextPos() const {
  gui::Font& font = gui::GetFont();
  // We use the same margin on all sides because it looks nicer with fully rounded corners.
  float margin = (kTextHeight - gui::kLetterSize) / 2;
  float text_width = font.MeasureText(*text);
  return Vec2(width - text_width - margin, margin);
}

Number::Number(double x)
    : value(x),
      digits{NumberButton(make_unique<Text>("0")), NumberButton(make_unique<Text>("1")),
             NumberButton(make_unique<Text>("2")), NumberButton(make_unique<Text>("3")),
             NumberButton(make_unique<Text>("4")), NumberButton(make_unique<Text>("5")),
             NumberButton(make_unique<Text>("6")), NumberButton(make_unique<Text>("7")),
             NumberButton(make_unique<Text>("8")), NumberButton(make_unique<Text>("9"))},
      dot(make_unique<Text>(".")),
      backspace(gui::MakeShapeWidget(kBackspaceShape, 0xff000000)),
      text("0"),
      text_field(*this) {
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
  return SkRRect::MakeRectXY(SkRect::MakeXYWH(0, 0, kWidth, kHeight), kCornerRadius, kCornerRadius);
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

ControlFlow Number::VisitChildren(gui::Visitor& visitor) {
  if (visitor(digits[0]) == ControlFlow::Stop) return ControlFlow::Stop;
  if (visitor(dot) == ControlFlow::Stop) return ControlFlow::Stop;
  if (visitor(backspace) == ControlFlow::Stop) return ControlFlow::Stop;
  for (int i = 0; i < 10; ++i) {
    if (visitor(digits[i]) == ControlFlow::Stop) return ControlFlow::Stop;
  }
  if (visitor(text_field) == ControlFlow::Stop) return ControlFlow::Stop;
  return ControlFlow::Continue;
}

SkMatrix Number::TransformToChild(const Widget& child, animation::Context&) const {
  auto cell = [](int row, int col) {
    float x = kBorderWidth + kAroundWidgetMargin + col * (kButtonWidth + kBetweenButtonsMargin);
    float y = kBorderWidth + kAroundWidgetMargin + row * (kButtonHeight + kBetweenButtonsMargin);
    return SkMatrix::Translate(-x, -y);
  };
  if (&child == &text_field) {
    return SkMatrix::Translate(-kBorderWidth - kAroundWidgetMargin,
                               -kHeight + kBorderWidth + kAroundWidgetMargin + kTextHeight);
  }
  if (&child == &digits[0]) {
    return cell(0, 0);
  }
  if (&child == &dot) {
    return cell(0, 1);
  }
  if (&child == &backspace) {
    return cell(0, 2);
  }
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      int digit = 3 * row + col + 1;
      if (&child == &digits[digit]) {
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
  LOG << "Action contact point is " << action->contact_point;
  return action;
}

}  // namespace automat::library
