// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_wayland_window.hpp"

#include <include/core/SkCanvas.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkImage.h>
#include <include/core/SkM44.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkSamplingOptions.h>
#include <include/effects/SkGradient.h>

#include "animation.hpp"
#include "drawing.hpp"
#include "font.hpp"
#include "keyboard.hpp"
#include "math.hpp"
#include "menu.hpp"
#include "pointer.hpp"
#include "root_widget.hpp"
#include "toy.hpp"
#include "ui_beta.hpp"
#include "units.hpp"
#include "wayland.hpp"

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
  auto pref = decoration_preference.load(std::memory_order_relaxed);
  if (pref != DecorationPreference::Auto) {
    StrView v = pref == DecorationPreference::ServerSide ? "server" : "client";
    writer.Key("decoration");
    writer.String(v.data(), v.size());
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
  if (key == "decoration") {
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
  return false;
}

namespace {

constexpr float kClientPx = 0.20_mm;  // one client pixel on the board
constexpr float kTitleH = 7_mm;
constexpr float kFrame = 5_mm;
constexpr float kContentRadius = 4_mm;
constexpr float kMinContentW = 3_cm;
constexpr float kMinContentH = 3_cm;

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

struct DecorationOption : TextOption {
  WeakPtr<WaylandWindow> window;
  WaylandWindow::DecorationPreference pref;
  Option::Dir dir;
  DecorationOption(Str label, WeakPtr<WaylandWindow> window,
                   WaylandWindow::DecorationPreference pref, Option::Dir dir)
      : TextOption(std::move(label)), window(window), pref(pref), dir(dir) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<DecorationOption>(text, window, pref, dir);
  }
  std::unique_ptr<Action> Activate(ui::Pointer&) const override {
    if (auto w = window.Lock()) {
      w->decoration_preference.store(pref, std::memory_order_relaxed);
      if (wayland::server) wayland::server->SendDecorationPreference(*w);
    }
    return nullptr;
  }
  Option::Dir PreferredDir() const override { return dir; }
};

struct DecorationMenuOption : TextOption, OptionsProvider {
  WeakPtr<WaylandWindow> window;
  DecorationMenuOption(WeakPtr<WaylandWindow> window)
      : TextOption("Decoration..."), window(window) {}
  std::unique_ptr<Option> Clone() const override {
    return std::make_unique<DecorationMenuOption>(window);
  }
  std::unique_ptr<Action> Activate(ui::Pointer& pointer) const override {
    return OpenMenu(pointer);
  }
  void VisitOptions(const OptionsVisitor& visitor) const override {
    using P = WaylandWindow::DecorationPreference;
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

Vec2 WindowBoardSize(int width, int height) {
  float content_w = width > 0 ? width * kClientPx : kMinContentW;
  float content_h = height > 0 ? height * kClientPx : kMinContentH;
  return Vec2(content_w + 2 * kFrame, content_h + 2 * kFrame + kTitleH);
}

struct WaylandSurfaceToy : ui::beta::ObjectToy, ui::PointerMoveCallback {
  sk_sp<SkImage> image_;
  SkRect src_crop_ = SkRect::MakeEmpty();
  SkISize dst_size_ = {};
  SkPath input_shape_;  // this surface's input region, in toy-local coordinates
  Vec<WaylandSurface::Child> below_, above_;

  WaylandSurfaceToy(ui::Widget* parent, Object& obj) : ui::beta::ObjectToy(parent, obj) {}

  Ptr<WaylandSurface> LockSurface() const { return LockObject<WaylandSurface>(); }

  // Child surfaces are placed relative to `TopLeft`.
  virtual Vec2 TopLeft() const { return Vec2(0, 0); }

  void PullSurfaceState() {
    auto s = LockSurface();
    if (!s) return;
    auto lock = std::lock_guard(s->mutex);
    image_ = s->image;
    src_crop_ = s->src_crop;
    dst_size_ = s->dst_size;
    below_ = s->below;
    above_ = s->above;
    // The input region arrives in client pixels (y-down from the top-left); map it
    // to toy-local coordinates (board metres, y-up) for Shape().
    input_shape_ = s->input_region.makeTransform(SkMatrix::Scale(kClientPx, -kClientPx));
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

  SkPath Shape() const override { return input_shape_; }
  Optional<Rect> TextureBounds() const override {
    float w = dst_size_.width() * kClientPx, h = dst_size_.height() * kClientPx;
    return Rect{0, -h, w, 0};
  }

  // Maps a toy-local point into this surface's client pixels, clamped to it.
  //
  // Returns boolean indicating whether point `l` was inside (no clamping was applied).
  bool ToSurfacePx(Vec2& l) const {
    l -= TopLeft();
    l.y = -l.y;
    l /= kClientPx;
    bool inside = true;
    if (l.x < 0) {
      inside = false;
      l.x = 0;
    } else if (l.x > dst_size_.width()) {
      inside = false;
      l.x = dst_size_.width();
    }
    if (l.y < 0) {
      inside = false;
      l.y = 0;
    } else if (l.y > dst_size_.height()) {
      inside = false;
      l.y = dst_size_.height();
    }
    return inside;
  }
  // Gives the keyboard to the toplevel window (keyboard focus is never on a
  // subsurface); walks up to the owning WaylandWindowToy. Defined below.
  void FocusWindow(ui::Pointer& p);

  void PointerOver(ui::Pointer& p) override {
    Vec2 px = p.PositionWithin(*this);
    ToSurfacePx(px);
    if (auto s = LockSurface()) wayland::server->SendPointerEnter(*s, px.x, px.y);
    StartWatching(p);
  }
  void PointerLeave(ui::Pointer& p) override {
    StopWatching(p);
    if (auto s = LockSurface()) wayland::server->SendPointerLeave(*s);
  }
  void PointerMove(ui::Pointer& p, Vec2) override {
    Vec2 px = p.PositionWithin(*this);
    ToSurfacePx(px);
    if (auto s = LockSurface()) wayland::server->SendPointerMotion(*s, px.x, px.y);
  }
  bool PointerWheel(ui::Pointer& p, float delta) override {
    if (!ui::RootWidget::kWaylandLock) return false;
    if (auto s = LockSurface()) wayland::server->SendPointerAxis(*s, delta);
    return true;
  }
  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override;

  void Draw(SkCanvas& canvas) const override {
    DrawSurfaceImage(canvas, image_, src_crop_, dst_size_, TopLeft());
  }
};

// The toy of a mapped toplevel: hand-drawn chrome (title band + frame) around
// the toplevel surface's content, which it draws and whose child surfaces it
// hosts (inherited from WaylandSurfaceToy). It also owns all input routing for
// the window tree: presses, motion, scroll and keys forwarded to the client.
struct WaylandWindowToy : WaylandSurfaceToy {
  Str title_;
  bool client_gone_ = false;
  bool decorate_ = false;
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
    decorate_ = win->server_side_decorated.load(std::memory_order_relaxed);
  }

  bool Decorated() const { return decorate_ || client_gone_ || !image_; }
  float Frame() const { return Decorated() ? kFrame : 0; }
  float TitleH() const { return Decorated() ? kTitleH : 0; }

  // Size of the main (toplevel) surface
  Vec2 ContentSize() const {
    return {dst_size_.width() > 0 ? dst_size_.width() * kClientPx : kMinContentW,
            dst_size_.height() > 0 ? dst_size_.height() * kClientPx : kMinContentH};
  }
  Rect ContentRect() const { return Rect::MakeAtZero(ContentSize()); }
  RRect ContentRRect() const { return RRect::MakeSimple(ContentRect(), kContentRadius); }
  RRect FrameMidRRect() const { return ContentRRect().Outset(kFrame * 3 / 8); }
  RRect FrameLightsRRect() const { return ContentRRect().Outset(kFrame * 11 / 16); }
  RRect FrameOutRRect() const { return ContentRRect().Outset(kFrame); }

  // The content area's top-left, where the toplevel surface (and its children)
  // sit, below the title band and inside the frame.
  Vec2 TopLeft() const override { return ContentRect().TopLeftCorner(); }

  Tock Tick(time::Timer&) override {
    PullState();
    SyncChildren(TopLeft());
    ApplyCursor();  // the client may have changed its cursor while the pointer sat still
    if (caret_) caret_->shape = FocusCaretShape();
    return Tock::Draw;
  }

  void ApplyCursor() {
    if (!hover_pointer_) return;
    Vec2 px = hover_pointer_->PositionWithin(*this);
    bool inside = ToSurfacePx(px);
    if (!inside) {
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

  // ---- client input routing (the content area belongs to the client) ----

  SkPath FocusCaretShape() const {
    if (!Decorated()) return SkPath();
    auto [w, h] = ContentSize().xy;
    float band_bottom = h / 2 - kTitleH;
    return SkPath::Rect(
        Rect{-w / 2 + 1.5_mm, band_bottom + 1.1_mm, w / 2 - 6_mm, band_bottom + 1.9_mm});
  }

  void FocusClient(ui::Pointer& p) {
    if (caret_ || !p.keyboard) return;
    auto [w, h] = ContentSize().xy;
    caret_ = &p.keyboard->RequestCaret(*this, Vec2(-w / 2 + Frame(), -h / 2 + Frame()));
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
    Vec2 px = p.PositionWithin(*this);
    ToSurfacePx(px);
    if (auto w = LockWindow()) wayland::server->SendPointerEnter(*w, px.x, px.y);
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
    Vec2 px = p.PositionWithin(*this);
    ToSurfacePx(px);
    if (auto w = LockWindow()) wayland::server->SendPointerMotion(*w, px.x, px.y);
    ApplyCursor();
  }

  bool PointerWheel(ui::Pointer& p, float delta) override {
    if (!ui::RootWidget::kWaylandLock) return false;
    Vec2 px = p.PositionWithin(*this);
    if (!ToSurfacePx(px)) return false;  // outside
    if (auto w = LockWindow()) wayland::server->SendPointerAxis(*w, delta);
    return true;
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override;

  void VisitOptions(const OptionsVisitor& visitor) const override {
    ObjectToy::VisitOptions(visitor);
    DecorationMenuOption deco{owner.Copy<WaylandWindow>()};
    visitor(deco);
  }

  SkPath Shape() const override {
    if (!Decorated()) return SkPath::Rect(ContentRect());
    return SkPath::RRect(FrameOutRRect());
  }
  Optional<Rect> TextureBounds() const override {
    auto rect = ContentRect().Outset(kFrame);
    rect.top += kTitleH * 1.25f;
    return rect;
  }

  void Draw(SkCanvas& canvas) const override {
    if (Decorated()) {
      auto frame_inner = ContentRRect();
      auto frame_mid = FrameMidRRect();
      auto frame_outer = FrameOutRRect();
      auto lights_rrect = FrameLightsRRect();

      auto font = ui::Font::MakeV2(ui::Font::GetBelanosimaRegular(), kTitleH);
      float w = font->MeasureText(title_);

      float one_pixel = 1.0f / canvas.getTotalMatrix().getScaleX();

      canvas.save();
      canvas.translate(-w / 2, frame_outer.rect.top - kTitleH * 0.1);
      SkPaint text_side_paint;
      text_side_paint.setColor("#3a2021"_color);
      text_side_paint.setStyle(SkPaint::kStrokeAndFill_Style);
      text_side_paint.setStrokeWidth(kTitleH * 0.2 / font->font_scale);
      font->DrawText(canvas, title_, text_side_paint);
      canvas.restore();

      SkPaint flat_border_paint;
      flat_border_paint.setColor("#9b252a"_color);
      canvas.drawDRRect(frame_outer, frame_mid, flat_border_paint);

      canvas.save();
      canvas.translate(-w / 2, frame_outer.rect.top);
      SkPaint text_outline_paint;
      text_outline_paint.setColor(flat_border_paint.getColor());
      text_outline_paint.setStyle(SkPaint::kStrokeAndFill_Style);
      text_outline_paint.setStrokeWidth(kTitleH * 0.2 / font->font_scale);
      font->DrawText(canvas, title_, text_outline_paint);
      canvas.restore();

      SkPaint bevel_border_paint;
      bevel_border_paint.setColor("#7d2627"_color);
      SetRRectShader(bevel_border_paint, frame_outer, "#3a2021"_color4f, "#7e2627"_color4f,
                     "#d86355"_color4f);

      canvas.drawDRRect(frame_mid.Outset(one_pixel), frame_inner.Outset(-one_pixel),
                        bevel_border_paint);
      canvas.save();
      canvas.clipRRect(frame_inner);
      DrawSurfaceImage(canvas, image_, src_crop_, dst_size_, TopLeft());
      canvas.restore();

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
            center, kLightRadius,
            SkGradient{SkGradient::Colors{bulb_colors, SkTileMode::kClamp}, {}}));

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
      font->DrawText(canvas, title_, title_paint);
      canvas.restore();
    } else {
      DrawSurfaceImage(canvas, image_, src_crop_, dst_size_, TopLeft());
    }
  }
};

uint32_t EvdevButtonCode(ui::ActionTrigger btn) {
  using ui::PointerButton;
  switch ((PointerButton)btn) {
    case PointerButton::Left:
      return 0x110;
    case PointerButton::Right:
      return 0x111;
    case PointerButton::Middle:
      return 0x112;
    default:
      return 0;
  }
}

void WaylandSurfaceToy::FocusWindow(ui::Pointer& p) {
  for (ui::Widget* w = this; w; w = w->parent)
    if (auto* wt = dynamic_cast<WaylandWindowToy*>(w)) {
      wt->FocusClient(p);
      return;
    }
}

// Held while a button initiated over a surface is down: routes press, motion and
// release to that surface's client (with the keyboard going to the toplevel)
// instead of dragging the window object.
struct ClientInputAction : Action {
  WaylandSurfaceToy& toy;
  uint32_t button;
  ClientInputAction(ui::Pointer& p, WaylandSurfaceToy& toy, uint32_t button)
      : Action(p), toy(toy), button(button) {
    toy.FocusWindow(p);
    Vec2 px = p.PositionWithin(toy);
    toy.ToSurfacePx(px);
    if (auto s = toy.LockSurface()) {
      wayland::server->SendPointerMotion(*s, px.x, px.y);
      wayland::server->SendPointerButton(*s, button, true);
    }
  }
  void Update() override {
    Vec2 px = pointer.PositionWithin(toy);
    toy.ToSurfacePx(px);
    if (auto s = toy.LockSurface()) wayland::server->SendPointerMotion(*s, px.x, px.y);
  }
  ~ClientInputAction() override {
    if (auto s = toy.LockSurface()) wayland::server->SendPointerButton(*s, button, false);
  }
};

static bool ForwardsToClient(ui::ActionTrigger btn) {
  return ui::RootWidget::kWaylandLock || (ui::PointerButton)btn == ui::PointerButton::Left;
}

std::unique_ptr<Action> WaylandSurfaceToy::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  if (ForwardsToClient(btn))
    if (uint32_t code = EvdevButtonCode(btn))
      return std::make_unique<ClientInputAction>(p, *this, code);
  return ObjectToy::FindAction(p, btn);
}

std::unique_ptr<Action> WaylandWindowToy::FindAction(ui::Pointer& p, ui::ActionTrigger btn) {
  if (ForwardsToClient(btn)) {
    if (uint32_t code = EvdevButtonCode(btn)) {
      Vec2 px = p.PositionWithin(*this);
      bool inside = ToSurfacePx(px);
      if (inside) return std::make_unique<ClientInputAction>(p, *this, code);
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
