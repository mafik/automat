#include "library_number.h"

#include <charconv>

#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include "color.h"
#include "font.h"
#include "gui_text.h"
#include "library_macros.h"

namespace automaton::library {

DEFINE_PROTO(Number);

static constexpr float kTextHeight = std::max(
    gui::kLetterSize + 2 * kMargin + 2 * kBorderWidth, kMinimalTouchableSize);
static constexpr float kButtonHeight = kMinimalTouchableSize;
static constexpr float kButtonRows = 4;
static constexpr float kRows = kButtonRows + 1;
static constexpr float kHeight = 2 * kBorderWidth + kTextHeight +
                                 kButtonRows * kButtonHeight +
                                 (kRows + 1) * kMargin;

static constexpr float kButtonWidth = kMinimalTouchableSize;
static constexpr float kButtonColumns = 3;
static constexpr float kWidth = 2 * kBorderWidth +
                                kButtonColumns * kButtonWidth +
                                (kButtonColumns + 1) * kMargin;

static constexpr float kCornerRadius =
    kMinimalTouchableSize / 2 + kMargin + kBorderWidth;

Number::Number(double x)
    : value(x), digits{gui::Button(this, std::make_unique<gui::Text>("0")),
                       gui::Button(this, std::make_unique<gui::Text>("1")),
                       gui::Button(this, std::make_unique<gui::Text>("2")),
                       gui::Button(this, std::make_unique<gui::Text>("3")),
                       gui::Button(this, std::make_unique<gui::Text>("4")),
                       gui::Button(this, std::make_unique<gui::Text>("5")),
                       gui::Button(this, std::make_unique<gui::Text>("6")),
                       gui::Button(this, std::make_unique<gui::Text>("7")),
                       gui::Button(this, std::make_unique<gui::Text>("8")),
                       gui::Button(this, std::make_unique<gui::Text>("9"))},
      dot(this, std::make_unique<gui::Text>(".")),
      backspace(this, std::make_unique<gui::Text>("<")),
      text_field(this, &text, kWidth - 2 * kMargin - 2 * kBorderWidth) {}

string_view Number::Name() const { return "Number"; }

std::unique_ptr<Object> Number::Clone() const {
  return std::make_unique<Number>(value);
}

string Number::GetText() const {
  char buffer[100];
  auto [end, ec] = std::to_chars(buffer, buffer + 100, value);
  *end = '\0';
  return buffer;
}

void Number::SetText(Location &error_context, string_view text) {
  value = std::stod(string(text));
}

void Number::Draw(SkCanvas &canvas, animation::State &animation_state) const {
  SkPath path = Shape();

  SkPaint paint;
  SkPoint pts[2] = {{0, 0}, {0, kHeight}};
  SkColor colors[2] = {0xff0f5f4d, 0xff468257};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(gradient);
  canvas.drawPath(path, paint);

  DrawChildren(canvas, animation_state);
}

SkPath Number::Shape() const {
  SkRRect rrect;
  constexpr float kUpperRadius =
      gui::kTextCornerRadius + kMargin + kBorderWidth;
  SkVector radii[4] = {{kCornerRadius, kCornerRadius},
                       {kCornerRadius, kCornerRadius},
                       {kUpperRadius, kUpperRadius},
                       {kUpperRadius, kUpperRadius}};
  rrect.setRectRadii(SkRect::MakeXYWH(0, 0, kWidth, kHeight), radii);
  static SkPath path = SkPath::RRect(rrect);
  return path;
}

gui::VisitResult Number::VisitImmediateChildren(gui::WidgetVisitor &visitor) {
  auto visit = [&](Widget &w, int row, int col) {
    float x = kBorderWidth + kMargin + col * (kButtonWidth + kMargin);
    float y = kBorderWidth + kMargin + row * (kButtonHeight + kMargin);
    SkMatrix down = SkMatrix::Translate(-x, -y);
    SkMatrix up = SkMatrix::Translate(x, y);
    auto result = visitor(w, down, up);
    if (result != gui::VisitResult::kContinue)
      return result;
    return result;
  };
  if (auto result = visit(digits[0], 0, 0);
      result != gui::VisitResult::kContinue)
    return result;
  if (auto result = visit(dot, 0, 1); result != gui::VisitResult::kContinue)
    return result;
  if (auto result = visit(backspace, 0, 2);
      result != gui::VisitResult::kContinue)
    return result;
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      int digit = 3 * row + col + 1;
      if (auto result = visit(digits[digit], row + 1, col);
          result != gui::VisitResult::kContinue)
        return result;
    }
  }
  if (auto result = visit(text_field, 4, 0);
      result != gui::VisitResult::kContinue)
    return result;
  return gui::VisitResult::kContinue;
}

} // namespace automaton::library
