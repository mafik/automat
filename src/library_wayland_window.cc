// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_wayland_window.hh"

#include <include/core/SkCanvas.h>
#include <include/core/SkImage.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkSamplingOptions.h>

#include "animation.hh"
#include "keyboard.hh"
#include "library_command.hh"
#include "pointer.hh"
#include "ui_slop.hh"
#include "units.hh"
#include "wayland_compositor.hh"

#if defined(__linux__)
#include "x11.hh"
#endif

namespace automat::library {

WaylandWindow::~WaylandWindow() {
#if defined(__linux__)
  // The only strong reference lives in this window's Location; its
  // destruction means the user deleted the window.
  wayland::NotifyWindowDestroyed(toplevel_handle.load());
#endif
}

void WaylandWindow::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  if (!recipe.empty()) {
    writer.Key("recipe");
    writer.StartArray();
    for (auto& w : recipe) {
      if (w.empty()) continue;
      writer.String(w.data(), w.size());
    }
    writer.EndArray();
  }
  if (!title.empty()) {
    writer.Key("title");
    writer.String(title.data(), title.size());
  }
}

bool WaylandWindow::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "recipe") {
    recipe.clear();
    for (auto i : ArrayView(d, status)) {
      (void)i;
      Str word;
      d.Get(word, status);
      if (OK(status)) recipe.push_back(std::move(word));
    }
    client_gone = true;
    pending_respawn = !recipe.empty();
    return true;
  }
  if (key == "title") {
    d.Get(title, status);
    return true;
  }
  return false;
}

namespace {

constexpr float kClientPx = 0.20_mm;  // one client pixel on the board
constexpr float kTitleH = 7_mm;
constexpr float kFrame = 0.8_mm;      // ink frame around the client pixels
constexpr float kMinContentW = 4_cm;  // placeholder size before the first buffer
constexpr float kMinContentH = 3_cm;

constexpr float kSlopPx = 7_cm / 480.f;  // slop kit pixel, as elsewhere

// Linux evdev button codes (input-event-codes.h), used by wl_pointer.button.
constexpr uint32_t kBtnLeft = 0x110;
constexpr uint32_t kBtnRight = 0x111;
constexpr uint32_t kBtnMiddle = 0x112;

struct SlopHere {
  SkCanvas& canvas;
  SlopHere(SkCanvas& c, SkPoint anchor_m) : canvas(c) {
    c.save();
    c.translate(anchor_m.fX, anchor_m.fY);
    c.scale(kSlopPx, -kSlopPx);
  }
  ~SlopHere() { canvas.restore(); }
};

}  // namespace

Vec2 WindowBoardSize(int width, int height) {
  float content_w = width > 0 ? width * kClientPx : kMinContentW;
  float content_h = height > 0 ? height * kClientPx : kMinContentH;
  return Vec2(content_w + 2 * kFrame, content_h + 2 * kFrame + kTitleH);
}

struct WaylandWindowToy : ui::slop::ObjectToy {
  sk_sp<SkImage> image;
  uint64_t image_serial = 0;
  Str title_;
  bool client_gone_ = false;
  float content_w = kMinContentW, content_h = kMinContentH;
  ui::Caret* caret_ = nullptr;  // present while the keyboard flows into the client

  WaylandWindowToy(ui::Widget* parent, Object& obj) : ui::slop::ObjectToy(parent, obj) {
    PullState();
  }
  ~WaylandWindowToy() override {
    if (caret_) caret_->Release();
  }

  Ptr<WaylandWindow> LockWindow() const { return LockObject<WaylandWindow>(); }

  void PullState() {
    if (auto win = LockWindow()) {
      auto lock = std::lock_guard(win->mutex);
      title_ = win->title.empty() ? win->app_id : win->title;
      client_gone_ = win->client_gone;
      if (win->width > 0 && win->height > 0) {
        content_w = win->width * kClientPx;
        content_h = win->height * kClientPx;
      }
      if (win->content_serial != image_serial) {
        image_serial = win->content_serial;
        image = win->content;
      }
    }
  }

  animation::Phase Tick(time::Timer&) override {
    PullState();
    return animation::Finished;
  }

  bool CenteredAtZero() const override { return true; }

  float TotalW() const { return content_w + 2 * kFrame; }
  float TotalH() const { return content_h + 2 * kFrame + kTitleH; }

  // Maps a toy-local point into client-surface pixels (clamped); returns
  // whether the point actually lies inside the content area.
  bool ClientPos(Vec2 local, float& sx, float& sy) const {
    float w = TotalW(), h = TotalH();
    float left = -w / 2 + kFrame, right = w / 2 - kFrame;
    float top = h / 2 - kTitleH - kFrame, bottom = -h / 2 + kFrame;
    bool inside = local.x >= left && local.x <= right && local.y >= bottom && local.y <= top;
    float cx = std::clamp(local.x, left, right);
    float cy = std::clamp(local.y, bottom, top);
    sx = (cx - left) / kClientPx;
    sy = (top - cy) / kClientPx;
    return inside;
  }

  // ---- client input routing (the content area belongs to the client) ----

  // The focus indicator is the system caret itself: on focus the familiar
  // blinking I-beam morphs into an underline inside the title band. Carets
  // draw black, so the shape must sit on guaranteed-light chrome - never on
  // client pixels, which are arbitrary and often dark.
  SkPath FocusCaretShape() const {
    float w = TotalW(), h = TotalH();
    float band_bottom = h / 2 - kTitleH;
    return SkPath::Rect(SkRect::MakeLTRB(-w / 2 + 1.5_mm, band_bottom + 1.1_mm, w / 2 - 6_mm,
                                         band_bottom + 1.9_mm));
  }

  void FocusClient(ui::Pointer& p) {
    if (caret_ || !p.keyboard) return;
    float w = TotalW(), h = TotalH();
    caret_ = &p.keyboard->RequestCaret(*this, Vec2(-w / 2 + kFrame, -h / 2 + kFrame));
    caret_->shape = FocusCaretShape();
    if (auto win = LockWindow()) wayland::SendKeyboardEnter(*win);
    WakeAnimation();
  }

  void ReleaseCaret(ui::Caret&) override {
    caret_ = nullptr;
    if (auto win = LockWindow()) wayland::SendKeyboardLeave(*win);
    WakeAnimation();
  }

  void ForwardKey(ui::Key key, bool pressed) {
#if defined(__linux__)
    uint32_t keycode = (uint32_t)x11::KeyToX11KeyCode(key.physical);
    if (keycode <= 8) return;
    if (auto win = LockWindow()) {
      // Wayland keyboards speak evdev; X11 keycodes are evdev + 8.
      wayland::SendKey(*win, keycode - 8, pressed, key.ctrl, key.alt, key.shift, key.windows);
    }
#endif
  }
  void KeyDown(ui::Caret&, ui::Key key) override { ForwardKey(key, true); }
  void KeyUp(ui::Caret&, ui::Key key) override { ForwardKey(key, false); }

  void PointerOver(ui::Pointer& p) override {
    float sx, sy;
    ClientPos(p.PositionWithin(*this), sx, sy);
    if (auto w = LockWindow()) wayland::SendPointerEnter(*w, sx, sy);
  }
  void PointerLeave(ui::Pointer&) override {
    if (auto w = LockWindow()) wayland::SendPointerLeave(*w);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override;

  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(TotalW(), TotalH()), 1.5_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(4_mm, 4_mm);
  }

  void Draw(SkCanvas& canvas) const override {
    float w = TotalW(), h = TotalH();
    float left = -w / 2, top = h / 2;
    // Hand-drawn chrome in slop pixels.
    {
      SlopHere g(canvas, {left, top});
      float w_px = w / kSlopPx;
      float h_px = h / kSlopPx;
      float title_px = kTitleH / kSlopPx;
      uint32_t seed = Seed(ui::slop::Hash2(0x3A11D, (uint32_t)(title_.size() + 1)));
      SkRect body = SkRect::MakeWH(w_px, h_px);
      SkPath base = ui::slop::WonkyRoundRect(body, 7.f, ui::slop::kWonk, seed);
      ui::slop::HandShadow(canvas, base, {ui::slop::kShadowDX, ui::slop::kShadowDY},
                           ui::slop::kShadow, seed);
      ui::slop::MisregFill(canvas, base, ui::slop::kPaperCream, seed);
      SkRect band = SkRect::MakeLTRB(0, 0, w_px, title_px);
      canvas.save();
      canvas.clipPath(base, true);
      canvas.drawPath(
          ui::slop::WobbleRect(band, ui::slop::kWonk, ui::slop::kSeg, ui::slop::Hash2(seed, 3)),
          ui::slop::InkPaint(ui::slop::kBlue, 1.f, true));
      SkPaint band_fill;
      band_fill.setColor(ui::slop::kBlue);
      band_fill.setAntiAlias(true);
      canvas.drawPath(
          ui::slop::WobbleRect(band, ui::slop::kWonk, ui::slop::kSeg, ui::slop::Hash2(seed, 3)),
          band_fill);
      canvas.restore();
      ui::slop::DrawTextIn(canvas, title_.empty() ? "Wayland Window" : title_,
                           SkRect::MakeLTRB(8, 0, w_px - 8, title_px), ui::slop::kBodySize,
                           ui::slop::TextOn(ui::slop::kBlue), ui::slop::TextAlign::Left, true,
                           seed);
      ui::slop::SketchyStroke(canvas, base, ui::slop::kInk, ui::slop::kStroke, seed, 2);
      if (client_gone_ || !image) {
        SkRect inner = SkRect::MakeLTRB(6, title_px + 4, w_px - 6, h_px - 6);
        ui::slop::HatchRect(canvas, inner, ui::slop::kGray, 11.f, seed);
      }
    }
    if (image) {
      // The client pixels, top row at the top: flip into +Y-down for drawing.
      float content_top = top - kTitleH - kFrame;
      float content_left = left + kFrame;
      canvas.save();
      canvas.scale(1, -1);
      SkRect dst = SkRect::MakeLTRB(content_left, -content_top, content_left + content_w,
                                    -(content_top - content_h));
      canvas.drawImageRect(image, dst, SkSamplingOptions(SkFilterMode::kLinear));
      canvas.restore();
    }
  }
};

// Held while a button initiated inside the content area is down: routes
// press, motion and release to the client instead of dragging the object.
struct ClientInputAction : Action {
  WaylandWindowToy& toy;
  uint32_t button;
  ClientInputAction(ui::Pointer& p, WaylandWindowToy& toy, uint32_t button, float sx, float sy)
      : Action(p), toy(toy), button(button) {
    toy.FocusClient(p);
    if (auto w = toy.LockWindow()) {
      wayland::SendPointerMotion(*w, sx, sy);
      wayland::SendPointerButton(*w, button, true);
    }
  }
  void Update() override {
    float sx, sy;
    toy.ClientPos(pointer.PositionWithin(toy), sx, sy);
    if (auto w = toy.LockWindow()) wayland::SendPointerMotion(*w, sx, sy);
  }
  ~ClientInputAction() override {
    if (auto w = toy.LockWindow()) wayland::SendPointerButton(*w, button, false);
  }
};

std::unique_ptr<Action> WaylandWindowToy::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  using ui::PointerButton;
  uint32_t code = btn == PointerButton::Left     ? kBtnLeft
                  : btn == PointerButton::Right  ? kBtnRight
                  : btn == PointerButton::Middle ? kBtnMiddle
                                                 : 0;
  if (code) {
    float sx, sy;
    if (ClientPos(p.PositionWithin(*this), sx, sy)) {
      return std::make_unique<ClientInputAction>(p, *this, code, sx, sy);
    }
  }
  // Title bar and frame: the standard object behaviors (drag, menu).
  return ObjectToy::FindAction(p, btn);
}

std::unique_ptr<ObjectToy> WaylandWindow::MakeToy(ui::Widget* parent) {
  return std::make_unique<WaylandWindowToy>(parent, *this);
}

}  // namespace automat::library
