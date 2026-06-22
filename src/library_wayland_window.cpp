// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_wayland_window.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkImage.h>
#include <include/core/SkM44.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkSamplingOptions.h>

#include "animation.hpp"
#include "keyboard.hpp"
#include "pointer.hpp"
#include "root_widget.hpp"
#include "toy.hpp"
#include "ui_beta.hpp"
#include "units.hpp"
#include "wayland_compositor.hpp"

#if defined(__linux__)
#include "x11.hpp"
#endif

namespace automat::library {

WaylandWindow::~WaylandWindow() {
#if defined(__linux__)
  // The only strong reference lives in this window's Location; its
  // destruction means the user deleted the window.
  if (wayland::server) wayland::server->NotifyWindowDestroyed(toplevel_handle.load());
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

// Linux evdev button codes (input-event-codes.h), used by wl_pointer.button.
constexpr uint32_t kBtnLeft = 0x110;
constexpr uint32_t kBtnRight = 0x111;
constexpr uint32_t kBtnMiddle = 0x112;

// wp_cursor_shape_device_v1 shape (stable protocol values) to the nearest icon.
ui::Pointer::IconType ShapeToIcon(uint32_t shape) {
  switch (shape) {
    case 4:  // pointer
      return ui::Pointer::kIconHand;
    case 9:   // text
    case 10:  // vertical_text
      return ui::Pointer::kIconIBeam;
    case 8:  // crosshair
      return ui::Pointer::kIconCrosshair;
    case 13:  // move
    case 16:  // grab
    case 17:  // grabbing
    case 32:  // all_scroll
    case 36:  // all_resize
      return ui::Pointer::kIconAllScroll;
    case 18:  // e_resize
    case 25:  // w_resize
    case 26:  // ew_resize
    case 30:  // col_resize
      return ui::Pointer::kIconResizeHorizontal;
    case 19:  // n_resize
    case 22:  // s_resize
    case 27:  // ns_resize
    case 31:  // row_resize
      return ui::Pointer::kIconResizeVertical;
    default:  // default and the long tail (help, wait, copy, ...)
      return ui::Pointer::kIconArrow;
  }
}

// Draws a surface's committed buffer, sampling `src` (buffer pixels) into a
// dst_size-pixel rectangle. The image is flipped to keep row 0 at the top.
// For opaque content (XRGB/XR24) alpha is set to 1.
void DrawSurfaceImage(SkCanvas& canvas, const sk_sp<SkImage>& image, const SkRect& src,
                      SkISize dst_size, Vec2 top_left) {
  if (!image) return;
  SkPaint paint;
  if (image->isOpaque())
    paint.setColorFilter(SkColorFilters::Blend(SK_ColorBLACK, SkBlendMode::kDstOver));
  float w = dst_size.width() * kClientPx, h = dst_size.height() * kClientPx;
  canvas.save();
  canvas.translate(top_left.x, top_left.y);
  canvas.scale(1, -1);
  canvas.drawImageRect(image, src, SkRect::MakeWH(w, h), SkSamplingOptions(SkFilterMode::kLinear),
                       &paint, SkCanvas::kStrict_SrcRectConstraint);
  canvas.restore();
}

}  // namespace

Vec2 WindowBoardSize(int width, int height) {
  float content_w = width > 0 ? width * kClientPx : kMinContentW;
  float content_h = height > 0 ? height * kClientPx : kMinContentH;
  return Vec2(content_w + 2 * kFrame, content_h + 2 * kFrame + kTitleH);
}

struct WaylandSurfaceToy : ui::beta::ObjectToy {
  sk_sp<SkImage> image_;
  SkRect src_crop_ = SkRect::MakeEmpty();
  SkISize dst_size_ = {};
  Vec<WaylandSurface::Child> below_, above_;
  uint64_t content_serial_ = 0;

  WaylandSurfaceToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {}

  Ptr<WaylandSurface> LockSurface() const { return LockObject<WaylandSurface>(); }

  // Child surfaces are placed relative to `TopLeft`.
  virtual Vec2 TopLeft() const { return Vec2(0, 0); }

  void PullSurfaceState() {
    auto s = LockSurface();
    if (!s) return;
    auto lock = std::lock_guard(s->mutex);
    if (s->content_serial == content_serial_) return;
    content_serial_ = s->content_serial;
    image_ = s->image;
    src_crop_ = s->src_crop;
    dst_size_ = s->dst_size;
    below_ = s->below;
    above_ = s->above;
  }

  void SyncChildren(Vec2 content_origin) {
    auto& toys = ToyStore();
    auto place = [&](WaylandSurface::Child& c, bool over) {
      auto& ct = toys.FindOrMake(*c.surface, this);
      Vec2 p = c.is_popup
                   ? PlacePopup(c, content_origin)
                   : content_origin + Vec2(c.offset.x() * kClientPx, -c.offset.y() * kClientPx);
      ct.local_to_parent = SkM44(SkMatrix::Translate(p.x, p.y));
      if (over)
        layers.OrderAbove(&ct);
      else
        layers.OrderBelow(&ct);
    };
    for (auto& c : below_) place(c, false);
    for (auto it = above_.rbegin(); it != above_.rend(); ++it) place(*it, true);
  }

  // Flips or slides (whichever the client permits) the popup to keep it inside
  // the viewport.
  Vec2 PlacePopup(const WaylandSurface::Child& c, Vec2 top_left) {
    auto to_local = [&](SkIPoint o) {
      return top_left + Vec2(o.x() * kClientPx, -o.y() * kClientPx);
    };
    Vec2 base = to_local(c.offset), flip = to_local(c.flipped);
    SkISize sz;
    {
      auto lock = std::lock_guard(c.surface->mutex);
      sz = c.surface->dst_size;
    }
    float w = sz.width() * kClientPx, h = sz.height() * kClientPx;
    if (w <= 0 || h <= 0) return base;
    ui::RootWidget& root = FindRootWidget();
    SkRect vp = ui::TransformBetween(root, *this).mapRect(root.Shape().getBounds());
    float vminx = std::min(vp.fLeft, vp.fRight), vmaxx = std::max(vp.fLeft, vp.fRight);
    float vminy = std::min(vp.fTop, vp.fBottom), vmaxy = std::max(vp.fTop, vp.fBottom);
    // The popup occupies [x, x + w] horizontally and [y - h, y] vertically (y = top).
    auto fits_x = [&](float x) { return x >= vminx && x + w <= vmaxx; };
    auto fits_y = [&](float y) { return y - h >= vminy && y <= vmaxy; };
    float x = base.x, y = base.y;
    if (!fits_x(x)) {
      if (c.flip_x && fits_x(flip.x)) x = flip.x;
      if (!fits_x(x) && c.slide_x) x = std::clamp(x, vminx, std::max(vminx, vmaxx - w));
    }
    if (!fits_y(y)) {
      if (c.flip_y && fits_y(flip.y)) y = flip.y;
      if (!fits_y(y) && c.slide_y) y = std::clamp(y, std::min(vminy + h, vmaxy), vmaxy);
    }
    return Vec2(x, y);
  }

  Tock Tick(time::Timer& timer) override {
    if (!LockSurface()) {
      MarkDead(timer.now);
      return {};
    }
    PullSurfaceState();
    SyncChildren(TopLeft());
    return Tock::Draw;
  }

  bool CenteredAtZero() const override { return false; }
  bool AllowChildPointerEvents(ui::Widget&) const override { return false; }

  SkPath Shape() const override {
    float w = dst_size_.width() * kClientPx, h = dst_size_.height() * kClientPx;
    return SkPath::Rect(Rect{0, -h, w, 0});
  }
  Optional<Rect> TextureBounds() const override { return Shape().getBounds(); }

  void Draw(SkCanvas& canvas) const override {
    DrawSurfaceImage(canvas, image_, src_crop_, dst_size_, TopLeft());
  }
};

// The toy of a mapped toplevel: hand-drawn chrome (title band + frame) around
// the toplevel surface's content, which it draws and whose child surfaces it
// hosts (inherited from WaylandSurfaceToy). It also owns all input routing for
// the window tree: presses, motion, scroll and keys forwarded to the client.
struct WaylandWindowToy : WaylandSurfaceToy, ui::PointerMoveCallback {
  Str title_;
  bool client_gone_ = false;
  ui::Caret* caret_ = nullptr;  // present while the keyboard flows into the client
  ui::Pointer* hover_pointer_ = nullptr;
  Optional<ui::Pointer::IconOverride> cursor_override_;
  ui::Pointer::IconType cursor_icon_ = ui::Pointer::kIconArrow;

  WaylandWindowToy(ui::Widget* parent, Object& obj) : WaylandSurfaceToy(parent, obj) {
    PullState();
  }
  ~WaylandWindowToy() override {
    if (caret_) caret_->Release();
  }

  Ptr<WaylandWindow> LockWindow() const { return LockObject<WaylandWindow>(); }

  void PullState() {
    PullSurfaceState();
    auto win = LockWindow();
    if (!win) return;
    auto lock = std::lock_guard(win->mutex);
    title_ = win->title.empty() ? win->app_id : win->title;
    client_gone_ = win->client_gone;
  }

  float ContentW() const {
    return dst_size_.width() > 0 ? dst_size_.width() * kClientPx : kMinContentW;
  }
  float ContentH() const {
    return dst_size_.height() > 0 ? dst_size_.height() * kClientPx : kMinContentH;
  }
  float TotalW() const { return ContentW() + 2 * kFrame; }
  float TotalH() const { return ContentH() + 2 * kFrame + kTitleH; }

  // The content area's top-left, where the toplevel surface (and its children)
  // sit, below the title band and inside the frame.
  Vec2 TopLeft() const override {
    return Vec2(-TotalW() / 2 + kFrame, TotalH() / 2 - kTitleH - kFrame);
  }

  Tock Tick(time::Timer&) override {
    PullState();
    SyncChildren(TopLeft());
    ApplyCursor();  // the client may have changed its cursor while the pointer sat still
    return Tock::Draw;
  }

  void ApplyCursor() {
    if (!hover_pointer_) return;
    float sx, sy;
    if (!ClientPos(hover_pointer_->PositionWithin(*this), sx, sy)) {
      cursor_override_.reset();
      return;
    }
    auto win = LockWindow();
    ui::Pointer::IconType want =
        win ? ShapeToIcon(win->cursor_shape.load(std::memory_order_relaxed))
            : ui::Pointer::kIconArrow;
    if (cursor_override_ && cursor_icon_ == want) return;
    cursor_icon_ = want;
    cursor_override_.emplace(*hover_pointer_, want);
  }

  bool CenteredAtZero() const override { return true; }

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
    return SkPath::Rect(
        Rect{-w / 2 + 1.5_mm, band_bottom + 1.1_mm, w / 2 - 6_mm, band_bottom + 1.9_mm});
  }

  void FocusClient(ui::Pointer& p) {
    if (caret_ || !p.keyboard) return;
    float w = TotalW(), h = TotalH();
    caret_ = &p.keyboard->RequestCaret(*this, Vec2(-w / 2 + kFrame, -h / 2 + kFrame));
    caret_->shape = FocusCaretShape();
    if (auto win = LockWindow()) wayland::server->SendKeyboardEnter(*win);
    WakeAnimation();
  }

  void ReleaseCaret(ui::Caret&) override {
    caret_ = nullptr;
    if (auto win = LockWindow()) wayland::server->SendKeyboardLeave(*win);
    WakeAnimation();
  }

  void ForwardKey(ui::Key key, bool pressed) {
#if defined(__linux__)
    uint32_t keycode = (uint32_t)x11::KeyToX11KeyCode(key.physical);
    if (keycode <= 8) return;
    if (auto win = LockWindow()) {
      // Wayland keyboards speak evdev; X11 keycodes are evdev + 8.
      wayland::server->SendKey(*win, keycode - 8, pressed, key.ctrl, key.alt, key.shift,
                               key.windows);
    }
#endif
  }
  void KeyDown(ui::Caret&, ui::Key key) override { ForwardKey(key, true); }
  void KeyUp(ui::Caret&, ui::Key key) override { ForwardKey(key, false); }

  void PointerOver(ui::Pointer& p) override {
    float sx, sy;
    ClientPos(p.PositionWithin(*this), sx, sy);
    if (auto w = LockWindow()) wayland::server->SendPointerEnter(*w, sx, sy);
    StartWatching(p);
    hover_pointer_ = &p;
    ApplyCursor();
  }
  void PointerLeave(ui::Pointer& p) override {
    StopWatching(p);
    cursor_override_.reset();
    hover_pointer_ = nullptr;
    if (auto w = LockWindow()) wayland::server->SendPointerLeave(*w);
  }
  void PointerMove(ui::Pointer& p, Vec2) override {
    float sx, sy;
    ClientPos(p.PositionWithin(*this), sx, sy);
    if (auto w = LockWindow()) wayland::server->SendPointerMotion(*w, sx, sy);
    ApplyCursor();
  }

  // Over the chrome, fall through (return false) so the board still zooms.
  bool PointerWheel(ui::Pointer& p, float delta) override {
    float sx, sy;
    if (!ClientPos(p.PositionWithin(*this), sx, sy)) return false;
    if (auto w = LockWindow()) wayland::server->SendPointerAxis(*w, delta);
    return true;
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
    uint32_t seed = Seed(ui::beta::Hash2(0x3A11D, (uint32_t)(title_.size() + 1)));
    Rect body = Rect::MakeCenterZero(w, h);
    SkPath base = ui::beta::WonkyRoundRect(body, 1.0_mm, ui::beta::kWonk, seed);
    ui::beta::HandShadow(canvas, base, {ui::beta::kShadowDX, -ui::beta::kShadowDY},
                         ui::beta::kShadow, seed);
    ui::beta::MisregFill(canvas, base, ui::beta::kPaperCream, seed);
    // The title band sits at the visual top.
    Rect band{-w / 2, h / 2 - kTitleH, w / 2, h / 2};
    canvas.save();
    canvas.clipPath(base, true);
    canvas.drawPath(
        ui::beta::WobbleRect(band, ui::beta::kWonk, ui::beta::kSeg, ui::beta::Hash2(seed, 3)),
        ui::beta::InkPaint(ui::beta::kBlue, 0.15_mm, true));
    SkPaint band_fill;
    band_fill.setColor(ui::beta::kBlue);
    band_fill.setAntiAlias(true);
    canvas.drawPath(
        ui::beta::WobbleRect(band, ui::beta::kWonk, ui::beta::kSeg, ui::beta::Hash2(seed, 3)),
        band_fill);
    canvas.restore();
    ui::beta::DrawTextIn(canvas, title_.empty() ? "Wayland Window" : title_,
                         Rect{-w / 2 + 1.2_mm, h / 2 - kTitleH, w / 2 - 1.2_mm, h / 2},
                         ui::beta::kBodySize, ui::beta::TextOn(ui::beta::kBlue),
                         ui::beta::TextAlign::Left, true, seed);
    ui::beta::SketchyStroke(canvas, base, ui::beta::kInk, ui::beta::kStroke, seed, 2);
    if (client_gone_ || !image_) {
      // The content area below the band, hatched while there is no buffer.
      Rect inner{-w / 2 + 0.9_mm, -h / 2 + 0.9_mm, w / 2 - 0.9_mm, h / 2 - kTitleH - 0.6_mm};
      ui::beta::HatchRect(canvas, inner, ui::beta::kGray, 1.6_mm, seed);
    }
    // The toplevel surface's own content; its child surfaces (subsurfaces and
    // popups) are separate widgets composited around it by the renderer.
    DrawSurfaceImage(canvas, image_, src_crop_, dst_size_, TopLeft());
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
      wayland::server->SendPointerMotion(*w, sx, sy);
      wayland::server->SendPointerButton(*w, button, true);
    }
  }
  void Update() override {
    float sx, sy;
    toy.ClientPos(pointer.PositionWithin(toy), sx, sy);
    if (auto w = toy.LockWindow()) wayland::server->SendPointerMotion(*w, sx, sy);
  }
  ~ClientInputAction() override {
    if (auto w = toy.LockWindow()) wayland::server->SendPointerButton(*w, button, false);
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

std::unique_ptr<ObjectToy> WaylandSurface::MakeToy(ui::Widget* parent) {
  return std::make_unique<WaylandSurfaceToy>(parent, *this);
}

std::unique_ptr<ObjectToy> WaylandWindow::MakeToy(ui::Widget* parent) {
  return std::make_unique<WaylandWindowToy>(parent, *this);
}

}  // namespace automat::library
