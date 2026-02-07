// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_number.hh"

#include <include/core/SkMatrix.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>
#include <rapidjson/rapidjson.h>

#include <charconv>

#include "drag_action.hh"
#include "number_text_field.hh"
#include "svg.hh"
#include "ui_button.hh"
#include "ui_constants.hh"
#include "ui_shape_widget.hh"
#include "ui_text.hh"
#include "widget.hh"

using namespace automat::ui;

namespace automat::library {

// Margin used between rows and columns of buttons
static constexpr float kBetweenButtonsMargin = kMargin;

// Margin used around the entire widget
static constexpr float kAroundWidgetMargin = kMargin * 2;

static constexpr float kBelowTextMargin = kMargin * 2;

// Height of the text field
static constexpr float kTextHeight =
    std::max(ui::kLetterSize + 2 * kBetweenButtonsMargin + 2 * kBorderWidth, kMinimalTouchableSize);
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

using ui::Text;

static const SkRRect kNumberRRect = [] {
  return SkRRect::MakeRectXY(SkRect::MakeXYWH(0, 0, kWidth, kHeight), kCornerRadius, kCornerRadius);
}();

static const SkRRect kNumberRRectInner = [] {
  SkRRect rrect;
  kNumberRRect.inset(kBorderWidth / 2, kBorderWidth / 2, &rrect);
  return rrect;
}();

static const SkPath kNumberShape = SkPath::RRect(kNumberRRect);

static const SkPaint kNumberBackgroundPaint = [] {
  SkPaint paint;
  SkPoint pts[2] = {{0, 0}, {0, kHeight}};
  SkColor colors[2] = {0xff483e37, 0xff6c5d53};
  sk_sp<SkShader> gradient =
      SkGradientShader::MakeLinear(pts, colors, nullptr, 2, SkTileMode::kClamp);
  paint.setShader(gradient);
  return paint;
}();

static const SkPaint kNumberBorderPaint = [] {
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

// Number Object methods

Number::Number(double x) : value(x) {}
Number::Number(const Number& other) : value(other.value) {}

string_view Number::Name() const { return "Number"; }
Ptr<Object> Number::Clone() const { return MAKE_PTR(Number, *this); }

string Number::GetText() const {
  char buffer[100];
  auto [end, ec] = std::to_chars(buffer, buffer + 100, value);
  *end = '\0';
  return buffer;
}

void Number::SetText(string_view text) {
  value = std::stod(string(text));
  WakeToys();
}

void Number::SerializeState(ObjectSerializer& writer) const {
  writer.Key("value");
  auto text = GetText();
  writer.RawValue(text.data(), text.size(), rapidjson::kNumberType);
}

bool Number::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "value") {
    Status status;
    d.Get(value, status);
    if (!OK(status)) {
      ReportError("Couldn't deserialize Number value: " + status.ToStr());
      return true;
    }
    WakeToys();
    return true;
  }
  return false;
}

// NumberButton

struct NumberButton : ui::Button {
  std::function<void(Location&)> activate;
  NumberButton(ui::Widget* parent, SkPath shape) : Button(parent) {
    child = std::make_unique<ShapeWidget>(this, shape);
    UpdateChildTransform();
  }
  NumberButton(ui::Widget* parent, std::string text) : Button(parent) {
    child = std::make_unique<Text>(this, text);
    UpdateChildTransform();
  }
  void Activate(ui::Pointer& pointer) override {
    Button::Activate(pointer);
    if (activate) {
      if (auto* lw = Closest<LocationWidget>(*pointer.hover)) {
        if (auto l = lw->LockLocation()) {
          activate(*l);
        }
      }
    }
  }
  StrView Name() const override { return "NumberButton"; }
  SkColor BackgroundColor() const override { return "#c8c4b7"_color; }
};

// NumberWidget

struct NumberWidget : Object::Toy {
  std::unique_ptr<NumberButton> digits[10];
  std::unique_ptr<NumberButton> dot;
  std::unique_ptr<NumberButton> backspace;
  std::unique_ptr<ui::NumberTextField> text_field;

  Ptr<Number> LockNumber() const { return LockObject<Number>(); }

  NumberWidget(ui::Widget* parent, Object& number_obj)
      : Object::Toy(parent, number_obj) {
    text_field = std::make_unique<ui::NumberTextField>(
        this, kWidth - 2 * kAroundWidgetMargin - 2 * kBorderWidth);
    dot = std::make_unique<NumberButton>(this, ".");
    backspace = std::make_unique<NumberButton>(this, PathFromSVG(kBackspaceShape));
    for (int i = 0; i < 10; ++i) {
      digits[i] = std::make_unique<NumberButton>(this, std::to_string(i));
      digits[i]->activate = [this, i](Location& l) {
        if (text_field->text.empty() || text_field->text == "0") {
          text_field->text = std::to_string(i);
        } else {
          text_field->text += std::to_string(i);
        }
        if (auto num = LockNumber()) {
          num->value = std::stod(text_field->text);
        }
        text_field->WakeAnimation();
        l.ScheduleUpdate();
      };
    }

    auto cell = [](int row, int col) {
      float x = kBorderWidth + kAroundWidgetMargin + col * (kButtonWidth + kBetweenButtonsMargin);
      float y = kBorderWidth + kAroundWidgetMargin + row * (kButtonHeight + kBetweenButtonsMargin);
      return SkM44::Translate(x, y);
    };
    text_field->local_to_parent =
        SkM44::Translate(kBorderWidth + kAroundWidgetMargin,
                         kHeight - kBorderWidth - kAroundWidgetMargin - kTextHeight);

    digits[0]->local_to_parent = cell(0, 0);
    dot->local_to_parent = cell(0, 1);
    backspace->local_to_parent = cell(0, 2);

    for (int row = 0; row < 3; ++row) {
      for (int col = 0; col < 3; ++col) {
        int digit = 3 * row + col + 1;
        digits[digit]->local_to_parent = cell(row + 1, col);
      }
    }

    dot->activate = [this](Location& l) {
      if (text_field->text.empty()) {
        text_field->text = "0";
      } else if (auto it = text_field->text.find('.'); it != std::string::npos) {
        text_field->text.erase(it, 1);
      }
      text_field->text += ".";
      while (text_field->text.size() > 1 && text_field->text[0] == '0' &&
             text_field->text[1] != '.') {
        text_field->text.erase(0, 1);
      }
      if (auto num = LockNumber()) {
        num->value = std::stod(text_field->text);
      }
      text_field->WakeAnimation();
      l.ScheduleUpdate();
    };
    backspace->activate = [this](Location& l) {
      if (!text_field->text.empty()) {
        text_field->text.pop_back();
      }
      if (text_field->text.empty()) {
        text_field->text = "0";
      }
      if (auto num = LockNumber()) {
        num->value = std::stod(text_field->text);
      }
      text_field->WakeAnimation();
      l.ScheduleUpdate();
    };

    // Initialize text_field from Object state
    if (auto num = LockNumber()) {
      text_field->text = num->GetText();
    }
  }

  animation::Phase Tick(time::Timer&) override {
    if (auto num = LockNumber()) {
      auto text = num->GetText();
      if (text_field->text != text) {
        text_field->text = text;
        text_field->WakeAnimation();
      }
    }
    return animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    canvas.drawRRect(kNumberRRectInner, kNumberBackgroundPaint);
    canvas.drawRRect(kNumberRRectInner, kNumberBorderPaint);
    DrawChildren(canvas);
  }

  SkPath Shape() const override { return kNumberShape; }
  bool CenteredAtZero() const override { return true; }

  void FillChildren(Vec<Widget*>& children) override {
    children.reserve(13);
    children.push_back(dot.get());
    children.push_back(backspace.get());
    for (int i = 0; i < std::size(digits); ++i) {
      children.push_back(digits[i].get());
    }
    children.push_back(text_field.get());
  }
};

std::unique_ptr<Object::Toy> Number::MakeToy(ui::Widget* parent) {
  return std::make_unique<NumberWidget>(parent, *this);
}

}  // namespace automat::library
