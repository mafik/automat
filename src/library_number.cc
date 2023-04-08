#include "library_number.h"

#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>

#include "library_macros.h"
#include "color.h"
#include "font.h"

namespace automaton::library {

DEFINE_PROTO(Number);

Number::Number(double x) : value(x) {}

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
  SkPoint pts[2] = {{0, 0}, {0, 0.01}};
  SkColor colors[2] = {SkColorFromHex("#0f5f4d"), SkColorFromHex("#468257")};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(gradient);
  canvas.drawPath(path, paint);

  SkPaint border_paint;
  border_paint.setStroke(true);
  border_paint.setStrokeWidth(0.00025);

  SkRRect rrect;
  if (path.isRRect(&rrect)) {
    float inset = border_paint.getStrokeWidth() / 2;
    rrect.inset(inset, inset);
    path = SkPath::RRect(rrect);
  }

  SkColor border_colors[2] = {SkColorFromHex("#1c5d3e"),
                              SkColorFromHex("#76a87a")};
  sk_sp<SkShader> border_gradient = SkGradientShader::MakeLinear(
      pts, border_colors, nullptr, 2, SkTileMode::kClamp);
  border_paint.setShader(border_gradient);

  canvas.drawPath(path, border_paint);

  SkPaint text_paint;
  text_paint.setColor(SK_ColorWHITE);

  SkRect path_bounds = path.getBounds();

  string text = GetText();

  canvas.save();
  canvas.translate(path_bounds.width() / 2 -
                       gui::GetFont().MeasureText(text) / 2,
                   path_bounds.height() / 2 - gui::kLetterSizeMM / 2 / 1000);
  gui::GetFont().DrawText(canvas, text, text_paint);
  canvas.restore();
}

SkPath Number::Shape() const {
  static SkPath path = SkPath::RRect(SkRRect::MakeRectXY(
      SkRect::MakeXYWH(0, 0, 0.008, 0.008), 0.001, 0.001));
  return path;
}

} // namespace automaton::library
