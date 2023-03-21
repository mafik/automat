#include "base.h"

#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkFont.h>
#include <include/core/SkFontMetrics.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathEffect.h>
#include <include/core/SkRRect.h>
#include <include/effects/SkGradientShader.h>
#include <unordered_map>

namespace automaton {

std::vector<const Object *> &Prototypes() {
  static std::vector<const Object *> prototypes;
  return prototypes;
}

void RegisterPrototype(const Object &prototype) {
  Prototypes().push_back(&prototype);
}

SkColor SkColorFromHex(const char *hex) {
  int r, g, b;
  sscanf(hex, "#%02x%02x%02x", &r, &g, &b);
  return SkColorSetRGB(r, g, b);
}

struct Font {
  SkFont sk_font;
  float font_scale;
  float line_thickness;

  static std::unique_ptr<Font> Make(float letter_size_mm) {
    constexpr float kMilimetersPerInch = 25.4;
    constexpr float kPointsPerInch = 72;
    // We want text to be `letter_size_mm` tall (by cap size).
    float letter_size_pt = letter_size_mm / kMilimetersPerInch * kPointsPerInch;
    float font_size_guess =
        letter_size_pt / 0.7f; // this was determined empirically
    // Create the font using the approximate size.
    SkFont sk_font(nullptr, font_size_guess, 1.f, 0.f);
    SkFontMetrics metrics;
    sk_font.getMetrics(&metrics);
    // The `fCapHeight` is the height of the capital letters.
    float font_scale = 0.001 * letter_size_mm / metrics.fCapHeight;
    float line_thickness = metrics.fUnderlineThickness * font_scale;
    return std::make_unique<Font>(sk_font, font_scale, line_thickness);
  }

  void DrawText(SkCanvas &canvas, string_view text, SkPaint &paint) {
    canvas.scale(font_scale, -font_scale);
    canvas.drawSimpleText(text.data(), text.size(), SkTextEncoding::kUTF8, 0, 0,
                          sk_font, paint);
    canvas.scale(1 / font_scale, -1 / font_scale);
  }

  float MeasureText(string_view text) {
    return sk_font.measureText(text.data(), text.size(),
                               SkTextEncoding::kUTF8) *
           font_scale;
  }
};

constexpr float kLetterSizeMM = 2;
constexpr float kLetterSize = kLetterSizeMM / 1000;

Font &GetFont() {
  static std::unique_ptr<Font> font = Font::Make(kLetterSizeMM);
  return *font;
}

constexpr float kBorderWidth = 0.00025;
constexpr float kFrameCornerRadius = 0.001;

void Object::Draw(const Location *here, SkCanvas &canvas) const {
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

  canvas.save();
  canvas.translate(path_bounds.width() / 2 - GetFont().MeasureText(Name()) / 2,
                   path_bounds.height() / 2 - kLetterSizeMM / 2 / 1000);
  GetFont().DrawText(canvas, Name(), text_paint);
  canvas.restore();
}

SkPath Object::Shape() const {
  static std::unordered_map<string_view, SkPath> basic_shapes;
  auto it = basic_shapes.find(Name());
  if (it == basic_shapes.end()) {
    constexpr float kNameMargin = 0.001;
    float width_name = GetFont().MeasureText(Name()) + 2 * kNameMargin;
    float width_rounded = ceil(width_name * 1000) / 1000;
    constexpr float kMinWidth = 0.008;
    float final_width = std::max(width_rounded, kMinWidth);
    SkRect rect = SkRect::MakeXYWH(0, 0, final_width, 0.008);
    SkRRect rrect = SkRRect::MakeRectXY(rect, 0.001, 0.001);
    it = basic_shapes.emplace(std::make_pair(Name(), SkPath::RRect(rrect)))
             .first;
  }
  return it->second;
}

SkColor SkColorBrighten(SkColor color) {
  float hsv[3];
  SkColorToHSV(color, hsv);
  hsv[2] = std::min(hsv[2] * 1.1f, 1.f);
  return SkHSVToColor(hsv);
}

SkColor SkColorDarken(SkColor color) {
  float hsv[3];
  SkColorToHSV(color, hsv);
  hsv[2] = std::max(hsv[2] * 0.9f, 0.f);
  return SkHSVToColor(hsv);
}

constexpr float kTextMargin = 0.001;
constexpr float kTextFieldHeight = 0.008; // 8mm
constexpr float kTextFieldMinWidth = kTextFieldHeight;

void DrawTextField(SkCanvas& canvas, string_view text) {
  Font& font = GetFont();
  float width = font.MeasureText(text);
  float width_margin = width + 2 * kTextMargin;
  float width_rounded = ceil(width_margin * 1000) / 1000;
  float final_width = std::max(width_rounded, kTextFieldMinWidth);
  SkRect rect = SkRect::MakeXYWH(0, 0, final_width, 0.008);
  SkPaint text_bg;
  text_bg.setColor(SK_ColorWHITE);
  text_bg.setAlphaf(0.5);
  canvas.drawRect(rect, text_bg);
  canvas.translate(kTextMargin, (kTextFieldHeight - kLetterSize) / 2);
  SkPaint underline;
  underline.setColor(SK_ColorBLACK);
  underline.setAlphaf(0.5);
  SkRect underline_rect = SkRect::MakeXYWH(0, -font.line_thickness, final_width - 2 * kTextMargin, - font.line_thickness);
  canvas.drawRect(underline_rect, underline);
  SkPaint text_fg;
  text_fg.setColor(SK_ColorBLACK);
  font.DrawText(canvas, text, text_fg);
}

void Location::Draw(SkCanvas &canvas) {
  if (object) {
    SkPath shape = object->Shape();
    SkRect bounds = shape.getBounds();
    bounds.outset(0.001, 0.001);
    bounds.inset(kBorderWidth/2, kBorderWidth/2);

    bounds.fBottom += kTextFieldHeight + 0.001;

    SkPaint frame_bg;
    SkColor frame_bg_colors[2] = {SkColorFromHex("#cccccc"),
                                  SkColorFromHex("#aaaaaa")};
    SkPoint gradient_pts[2] = {{0, bounds.bottom()}, {0, bounds.top()}};
    sk_sp<SkShader> frame_bg_shader = SkGradientShader::MakeLinear(
        gradient_pts, frame_bg_colors, nullptr, 2, SkTileMode::kClamp);
    frame_bg.setShader(frame_bg_shader);
    canvas.drawRoundRect(bounds, kFrameCornerRadius, kFrameCornerRadius, frame_bg);

    SkPaint frame_border;
    SkColor frame_border_colors[2] = {SkColorBrighten(frame_bg_colors[0]),
                                      SkColorDarken(frame_bg_colors[1])};
    sk_sp<SkShader> frame_border_shader = SkGradientShader::MakeLinear(
        gradient_pts, frame_border_colors, nullptr, 2, SkTileMode::kClamp);
    frame_border.setShader(frame_border_shader);
    frame_border.setStyle(SkPaint::kStroke_Style);
    frame_border.setStrokeWidth(0.00025);
    canvas.drawRoundRect(bounds, kFrameCornerRadius, kFrameCornerRadius, frame_border);

    object->Draw(this, canvas);

    canvas.translate(bounds.left() + 0.001, bounds.bottom() - kTextFieldHeight - 0.001);

    DrawTextField(canvas, object->Name());
  }
}

void Machine::DrawContents(SkCanvas &canvas) {
  SkRect clip = canvas.getLocalClipBounds();

  for (auto &loc : locations) {
    canvas.save();
    canvas.translate(loc->position.X, loc->position.Y);
    loc->Draw(canvas);
    canvas.restore();
  }
}

Argument::FinalLocationResult
Argument::GetFinalLocation(Location &here,
                           std::source_location source_location) const {
  FinalLocationResult result(GetObject(here, source_location));
  if (auto live_object = dynamic_cast<LiveObject *>(result.object)) {
    result.final_location = live_object->here;
  }
  return result;
}

void *Location::Nearby(function<void *(Location &)> callback) {
  if (auto parent_machine = ParentAs<Machine>()) {
    // TODO: sort by distance
    for (auto &other : parent_machine->locations) {
      if (auto ret = callback(*other)) {
        return ret;
      }
    }
  }
  return nullptr;
}

bool Location::HasError() {
  if (error != nullptr)
    return true;
  if (auto machine = ThisAs<Machine>()) {
    if (!machine->children_with_errors.empty())
      return true;
  }
  return false;
}
Error *Location::GetError() {
  if (error != nullptr)
    return error.get();
  if (auto machine = ThisAs<Machine>()) {
    if (!machine->children_with_errors.empty())
      return (*machine->children_with_errors.begin())->GetError();
  }
  return nullptr;
}
void Location::ClearError() {
  if (error == nullptr) {
    return;
  }
  error.reset();
  if (auto machine = ParentAs<Machine>()) {
    machine->ClearChildError(*this);
  }
}

Object *Location::Follow() {
  if (Pointer *ptr = object->AsPointer()) {
    return ptr->Follow(*this);
  }
  return object.get();
}

void Location::Put(unique_ptr<Object> obj) {
  if (object == nullptr) {
    object = std::move(obj);
    return;
  }
  if (Pointer *ptr = object->AsPointer()) {
    ptr->Put(*this, std::move(obj));
  } else {
    object = std::move(obj);
  }
}

unique_ptr<Object> Location::Take() {
  if (Pointer *ptr = object->AsPointer()) {
    return ptr->Take(*this);
  }
  return std::move(object);
}

Connection *Location::ConnectTo(Location &other, string_view label,
                                Connection::PointerBehavior pointer_behavior) {
  if (LiveObject *live_object = ThisAs<LiveObject>()) {
    live_object->Args([&](LiveArgument &arg) {
      if (arg.name == label &&
          arg.precondition >= Argument::kRequiresConcreteType) {
        std::string error;
        arg.CheckRequirements(*this, &other, other.object.get(), error);
        if (error.empty()) {
          pointer_behavior = Connection::kTerminateHere;
        }
      }
    });
  }
  Connection *c = new Connection(*this, other, pointer_behavior);
  outgoing.emplace(label, c);
  other.incoming.emplace(label, c);
  object->ConnectionAdded(*this, label, *c);
  return c;
}

void Location::ScheduleRun() { (new RunTask(this))->Schedule(); }

void Location::ScheduleLocalUpdate(Location &updated) {
  (new UpdateTask(this, &updated))->Schedule();
}

void Location::ScheduleErrored(Location &errored) {
  (new ErroredTask(this, &errored))->Schedule();
}

Argument then_arg("then", Argument::kOptional);
const Machine Machine::proto;

int log_executed_tasks = 0;

LogTasksGuard::LogTasksGuard() { ++log_executed_tasks; }
LogTasksGuard::~LogTasksGuard() { --log_executed_tasks; }

std::deque<unique_ptr<Task>> queue;
std::unordered_set<Location *> no_scheduling;
std::shared_ptr<Task> *global_then = nullptr;

channel events;

void RunThread() {
  while (true) {
    RunLoop();
    std::unique_ptr<Task> task = events.recv<Task>();
    if (task) {
      task.release()->Schedule();
    }
  }
}
void RunLoop(const int max_iterations) {
  if (log_executed_tasks) {
    LOG() << "RunLoop(" << queue.size() << " tasks)";
    LOG_Indent();
  }
  int iterations = 0;
  while (!queue.empty() &&
         (max_iterations < 0 || iterations < max_iterations)) {
    queue.front()->Execute();
    queue.pop_front();
    ++iterations;
  }
  if (log_executed_tasks) {
    LOG_Unindent();
  }
}
bool NoScheduling(Location *location) {
  return no_scheduling.find(location) != no_scheduling.end();
}
} // namespace automaton