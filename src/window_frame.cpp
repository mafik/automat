// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "window_frame.hpp"

#include <include/core/SkColorFilter.h>
#include <include/core/SkPathUtils.h>
#include <include/effects/SkGradient.h>
#include <include/pathops/SkPathOps.h>
#include <include/utils/SkTextUtils.h>

#include "board.hpp"
#include "color.hpp"
#include "drag_action.hpp"
#include "drawing.hpp"
#include "font.hpp"
#include "launcher.hpp"
#include "location.hpp"
#include "object.hpp"
#include "pointer.hpp"
#include "root_widget.hpp"
#include "vm.hpp"

namespace automat {

namespace {

struct DecorationOption : TextOption {
  WeakPtr<Object> window;
  DecoratedWindow::DecorationPreference pref;
  Option::Dir dir;
  DecorationOption(Str label, WeakPtr<Object> window, DecoratedWindow::DecorationPreference pref,
                   Option::Dir dir)
      : TextOption(std::move(label)), window(window), pref(pref), dir(dir) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<DecorationOption>(text, window, pref, dir);
  }
  std::unique_ptr<Action> Activate(ui::Pointer&) const override {
    if (auto obj = window.Lock())
      if (auto* w = dynamic_cast<DecoratedWindow*>(obj.get())) {
        w->decoration_preference.store(pref, std::memory_order_relaxed);
        w->DecorationPreferenceChanged();
      }
    return nullptr;
  }
  Option::Dir PreferredDir() const override { return dir; }
};

struct DecorationMenuOption : TextOption, OptionsProvider {
  WeakPtr<Object> window;
  DecorationMenuOption(WeakPtr<Object> window) : TextOption("Decoration..."), window(window) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<DecorationMenuOption>(window);
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    return OpenMenu(pointer);
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    using P = DecoratedWindow::DecorationPreference;
    DecorationOption automat_auto("Auto", window, P::Auto, Option::S);
    visitor(automat_auto);
    DecorationOption server_side("Automat", window, P::ServerSide, Option::W);
    visitor(server_side);
    DecorationOption client_side("App", window, P::ClientSide, Option::E);
    visitor(client_side);
  }
  Option::Dir PreferredDir() const override { return Option::S; }
};

}  // namespace

void VisitDecorationOptions(const WeakPtr<Object>& window, const OptionsVisitor& visitor) {
  DecorationMenuOption deco(window);
  visitor(deco);
}

void DecoratedWindow::SerializeDecoration(ObjectSerializer& writer) const {
  auto pref = decoration_preference.load(std::memory_order_relaxed);
  if (pref != DecorationPreference::Auto) {
    StrView v = pref == DecorationPreference::ServerSide ? "server" : "client";
    writer.Key("decoration");
    writer.String(v.data(), v.size());
  }
}

bool DecoratedWindow::DeserializeDecoration(ObjectDeserializer& d, StrView key) {
  if (key != "decoration") return false;
  Status status;
  Str v;
  d.Get(v, status);
  DecorationPreference pref = DecorationPreference::Auto;
  if (v == "server")
    pref = DecorationPreference::ServerSide;
  else if (v == "client")
    pref = DecorationPreference::ClientSide;
  decoration_preference.store(pref, std::memory_order_relaxed);
  return true;
}

void ClientInputActionBase::LinkWindow(Object& obj) {
  if (auto* w = dynamic_cast<DecoratedWindow*>(&obj)) {
    window = obj.AcquireWeakPtr();
    w->input_action = this;
  }
}

ClientInputActionBase::~ClientInputActionBase() {
  if (auto obj = window.Lock())
    if (auto* w = dynamic_cast<DecoratedWindow*>(obj.get()))
      if (w->input_action == this) w->input_action = nullptr;
}

bool StartClientMove(DecoratedWindow& window) {
  auto* input = window.input_action;
  if (!input) return false;
  auto* lw = Closest<LocationWidget>(input->InitiatingWidget());
  if (!lw) return false;
  auto loc = lw->LockLocation();
  if (!loc) return false;
  auto board = loc->LockBoard();
  if (!board) return false;
  ui::Pointer& pointer = input->pointer;
  auto* mw = pointer.root_widget.toys.FindOrNull(*board);
  if (!mw) return false;
  pointer.ReplaceAction(*input,
                        std::make_unique<DragLocationAction>(pointer, mw->DragStack(*loc), *mw));
  return true;
}

using library::ClientWindow;

ClientWindowToy::~ClientWindowToy() {
  if (caret_) caret_->Release();
}

Ptr<ClientWindow> ClientWindowToy::LockClientWindow() const { return LockObject<ClientWindow>(); }

void ClientWindowToy::PullState() {
  auto win = LockClientWindow();
  if (!win) return;
  auto lock = std::lock_guard(win->mutex);
  title_ = win->title.empty() ? win->app_id : win->title;
  client_gone_ = win->client_gone;
  client_decorated_ = win->client_decorated;
  pref_ = win->decoration_preference.load(std::memory_order_relaxed);
  image_ = win->image;
  content_size_ = win->content_size;
  PullMore(*win);
}

bool ClientWindowToy::Decorated() const {
  using P = DecoratedWindow::DecorationPreference;
  if (client_gone_) return true;
  if (pref_ != P::Auto) return pref_ == P::ServerSide;
  return !client_decorated_;
}

Vec2 ClientWindowToy::ContentSize() const {
  return {content_size_.width() > 0 ? content_size_.width() * kPx : kMinContent,
          content_size_.height() > 0 ? content_size_.height() * kPx : kMinContent};
}

ui::WindowFrame ClientWindowToy::Chrome() const { return {ContentSize(), title_}; }

SkPath ClientWindowToy::FocusCaretShape() const {
  return Decorated() ? Chrome().FocusCaretShape() : SkPath();
}

// Board coordinates to a point on the client surface, in client pixels.
Vec2 ClientWindowToy::ToSurfacePx(Vec2 local) const {
  Vec2 sz = ContentSize();
  local += Vec2(sz.x / 2, sz.y / 2);
  local.y = sz.y - local.y;
  return local / kPx;
}

SkPath ClientWindowToy::Shape() const {
  if (!Decorated()) return SkPath::Rect(ContentRect());
  return Chrome().Shape();
}

ui::Tock ClientWindowToy::Tick(time::Timer& timer) {
  if (!LockClientWindow()) {
    MarkDead(timer.now);
    return {};
  }
  Str prev_title = title_;
  SkISize prev_size = content_size_;
  bool prev_decorated = Decorated();
  PullState();
  bool extra_shape = TickMore(timer);
  if (caret_) caret_->shape = FocusCaretShape();
  Tock tock = Tock::Draw;
  if (title_ != prev_title || content_size_ != prev_size || Decorated() != prev_decorated ||
      extra_shape)
    tock |= Tock::Shape;
  return tock;
}

void ClientWindowToy::Draw(SkCanvas& canvas) const {
  Vec2 sz = ContentSize();
  Rect content = ContentRect();
  if (image_) {
    SkPaint paint;
    if (image_->isOpaque())
      paint.setColorFilter(SkColorFilters::Blend(SK_ColorBLACK, SkBlendMode::kDstOver));
    canvas.save();
    canvas.translate(content.left, content.top);
    canvas.scale(1, -1);
    canvas.drawImageRect(image_, SkRect::MakeWH(sz.x, sz.y),
                         SkSamplingOptions(SkFilterMode::kLinear), &paint);
    canvas.restore();
  } else {
    SkPaint background;
    background.setColor("#202020"_color);
    canvas.drawRect(content, background);
  }
  if (Decorated()) Chrome().Draw(canvas);
}

void ClientWindowToy::VisitOptions(const OptionsVisitor& visitor) const {
  ObjectToy::VisitOptions(visitor);
  VisitDecorationOptions(owner, visitor);
}

void ClientWindowToy::FocusClient(ui::Pointer& p) {
  if (caret_ || !p.keyboard) return;
  auto [w, h] = ContentSize().xy;
  caret_ = &p.keyboard->RequestCaret(*this, Vec2(-w / 2, h / 2));
  caret_->shape = FocusCaretShape();
  SendFocus(true);
  WakeAnimation();
}

void ClientWindowToy::ReleaseCaret(ui::Caret&) {
  caret_ = nullptr;
  SendFocus(false);
  WakeAnimation();
}

void ClientWindowToy::PointerMove(ui::Pointer& p, Vec2) {
  SendMotion(ToSurfacePx(p.PositionWithin(*this)));
}

void ClientWindowToy::PointerEnter(ui::Pointer& p) {
  SendCrossing(true, ToSurfacePx(p.PositionWithin(*this)));
  StartWatching(p);
}

void ClientWindowToy::PointerLeave(ui::Pointer& p) {
  SendCrossing(false, {});
  StopWatching(p);
}

void ClientWindowToy::KeyDown(ui::Caret&, ui::Key key) { SendKey(key, true); }

void ClientWindowToy::KeyUp(ui::Caret&, ui::Key key) { SendKey(key, false); }

std::unique_ptr<Action> ClientWindowToy::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  if ((ui::PointerButton)btn == ui::PointerButton::Left &&
      ContentRect().Contains(p.PositionWithin(*this)) && AllowClientPress(p)) {
    return BeginClientPress(p);
  }
  return ObjectToy::FindAction(p, btn);
}

ClientArrivals::ClientArrivals() = default;

ClientArrivals::~ClientArrivals() = default;

void ClientArrivals::Process() {
  for (auto& weak : move_requests) {
    if (auto win = weak.Lock()) StartClientMove(*win);
  }
  static int spawn_count = 0;
  for (auto& [w, launch] : appeared) {
    auto& win = *w;
    Ptr<Object> source = launch ? launch->source.Lock() : nullptr;
    Location* source_location = source ? source->MyLocation() : nullptr;
    if (source && !win.launcher->IsConnected()) {
      win.launcher->Connect(Interface(source.get(), nullptr));
    }
    Board* board = source_location ? source_location->LockBoard().get() : nullptr;
    if (!board) board = &DefaultBoard();
    auto& loc = board->CreateEmpty();
    if (source_location) {
      loc.placement = Location::PlaceBeside{source_location->AcquireWeakPtr()};
    } else {
      int n = spawn_count++;
      loc.placement = Location::Direct{Vec2(0.01f * (n % 3), -0.02f * (n % 5))};
    }
    loc.InsertHere(std::move(w));
    board->WakeToys();
    vm.WakeToys();
  }
  appeared.clear();
  for (auto& w : disappeared) {
    auto vm_lock = std::lock_guard(vm.mutex);
    for (auto& board : vm.boards) {
      if (auto* here = board->LocationOrNull(*w)) {
        board->Extract(*here);
        board->WakeToys();
      }
    }
    vm.WakeToys();
  }
  disappeared.clear();
  move_requests.clear();
}

}  // namespace automat

namespace automat::ui {

Font& WindowFrame::GetFont() {
  static auto font = Font::MakeV2(Font::GetBelanosimaRegular(), kTitleH);
  return *font;
}

SkPath WindowFrame::Shape() const {
  auto& font = GetFont();
  float w = font.sk_font.measureText(title.data(), title.size(), SkTextEncoding::kUTF8);
  SkPath title_fill;
  SkTextUtils::GetPath(title.data(), title.size(), SkTextEncoding::kUTF8, -w / 2, 0, font.sk_font,
                       &title_fill);

  SkPaint paint;
  paint.setStyle(SkPaint::kStroke_Style);
  // 0.3 is larger than 0.2 used for the real outline - this is to help in filling the holes in
  // the text
  paint.setStrokeWidth(kTitleH * 0.3 / font.font_scale);
  SkPath title_outline = skpathutils::FillPathWithPaint(title_fill, paint);

  SkPath s = title_fill;
  if (auto uni = Op(title_fill, title_outline, SkPathOp::kUnion_SkPathOp)) s = *uni;
  if (auto shift = Op(s, s.makeOffset(0, 2_mm / font.font_scale), SkPathOp::kUnion_SkPathOp)) {
    s = *shift;
  }
  auto frame = OutRRect();
  auto matrix = SkMatrix::ScaleTranslate(font.font_scale, -font.font_scale, 0, frame.rect.top);
  if (auto with_frame =
          Op(s.makeTransform(matrix), SkPath::RRect(frame), SkPathOp::kUnion_SkPathOp)) {
    s = *with_frame;
  }
  return s;
}

SkPath WindowFrame::FocusCaretShape() const {
  auto [w, h] = content_size.xy;
  float band_bottom = h / 2 - kTitleH;
  return SkPath::Rect(
      Rect{-w / 2 + 1.5_mm, band_bottom + 1.1_mm, w / 2 - 6_mm, band_bottom + 1.9_mm});
}

void WindowFrame::Draw(SkCanvas& canvas) const {
  auto frame_inner = ContentRRect();
  auto frame_mid = MidRRect();
  auto frame_outer = OutRRect();
  auto lights_rrect = LightsRRect();

  auto& font = GetFont();

  float w = font.MeasureText(title);

  float one_pixel = 1.0f / canvas.getTotalMatrix().getScaleX();

  canvas.save();
  canvas.translate(-w / 2, frame_outer.rect.top - kTitleH * 0.1);
  SkPaint title_side_paint;
  title_side_paint.setColor("#3a2021"_color);
  title_side_paint.setStyle(SkPaint::kStrokeAndFill_Style);
  title_side_paint.setStrokeWidth(kTitleH * 0.2 / font.font_scale);
  font.DrawText(canvas, title, title_side_paint);
  canvas.restore();

  SkPaint flat_border_paint;
  flat_border_paint.setColor("#9b252a"_color);
  canvas.drawDRRect(frame_outer, frame_mid, flat_border_paint);

  canvas.save();
  canvas.translate(-w / 2, frame_outer.rect.top);
  SkPaint text_outline_paint;
  text_outline_paint.setColor(flat_border_paint.getColor());
  text_outline_paint.setStyle(SkPaint::kStrokeAndFill_Style);
  text_outline_paint.setStrokeWidth(kTitleH * 0.2 / font.font_scale);
  font.DrawText(canvas, title, text_outline_paint);
  canvas.restore();

  SkPaint bevel_border_paint;
  bevel_border_paint.setColor("#7d2627"_color);
  SetRRectShader(bevel_border_paint, frame_outer, "#3a2021"_color4f, "#7e2627"_color4f,
                 "#d86355"_color4f);

  canvas.drawDRRect(frame_mid.Outset(one_pixel), frame_inner.Outset(-one_pixel),
                    bevel_border_paint);

  {  // Lights

    constexpr int kNumLights = 4 * 6;
    Vec2 light_positions[kNumLights];
    lights_rrect.EquidistantPoints(light_positions);
    Vec2 center{};
    constexpr float kLightRange = 5_mm;
    constexpr float kLightRadius = 1_mm;

    SkColor4f bulb_colors[] = {
        "#ffffa2"_color4f,  // light center
        "#ffff70"_color4f,  // light mid
        "#ffff93"_color4f,  // outer light edge (faint yellow)
    };
    SkPaint bulb_paint;
    bulb_paint.setShader(SkShaders::RadialGradient(
        center, kLightRadius, SkGradient{SkGradient::Colors{bulb_colors, SkTileMode::kClamp}, {}}));

    SkColor4f glow_colors[] = {
        "#5b0e00"_color4f,    // shadow
        "#5b0e00"_color4f,    // shadow
        "#ec4329"_color4f,    // warm red
        "#ec432980"_color4f,  // half-transparent warm red
        "#ec432900"_color4f,  // transparent warm red
    };
    SkPaint glow_paint;
    float glow_positions[] = {0, kLightRadius / kLightRange, kLightRadius * 1.1 / kLightRange,
                              kLightRadius * 2 / kLightRange, 1};
    glow_paint.setShader(SkShaders::RadialGradient(
        center, kLightRange,
        SkGradient{SkGradient::Colors{glow_colors, glow_positions, SkTileMode::kClamp}, {}}));
    canvas.save();
    canvas.clipRRect(frame_outer);
    canvas.clipRRect(frame_mid, SkClipOp::kDifference);
    for (int i = 0; i < kNumLights; ++i) {
      canvas.save();
      canvas.translate(light_positions[i].x, light_positions[i].y);
      canvas.drawCircle(0, 0, kLightRange, glow_paint);
      canvas.drawCircle(0, 0, kLightRadius, bulb_paint);
      canvas.restore();
    }
    canvas.restore();
  }

  SkPaint title_paint;
  title_paint.setColor("#e7e5cd"_color);
  canvas.save();
  canvas.translate(-w / 2, frame_outer.rect.top);
  font.DrawText(canvas, title, title_paint);
  canvas.restore();
}

}  // namespace automat::ui
