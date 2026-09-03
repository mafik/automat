// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot
#include "library_data_offer.hpp"

#include <include/core/SkCanvas.h>

#include <unordered_map>

#include "format.hpp"
#include "object.hpp"
#include "textures.hpp"
#include "ui_beta.hpp"
#include "units.hpp"
#include "xdg_icon.hpp"

#pragma comment(lib, "skia")

namespace automat::library {

using namespace ui::beta;

static SkPaint Fill(SkColor c) {
  SkPaint p;
  p.setAntiAlias(true);
  p.setColor(c);
  return p;
}

DataOffer::Entry* DataOffer::Add(StrView name) {
  uint32_t n = entry_count.load(std::memory_order_relaxed);
  if (n >= kMaxEntries) return nullptr;
  entries[n].name = Str(name);
  entry_count.store(n + 1, std::memory_order_release);
  WakeToys();
  return &entries[n];
}

void DataOffer::Complete(Entry& e, bool success) {
  e.failed.store(!success, std::memory_order_relaxed);
  e.done.store(true, std::memory_order_release);
  WakeToys();
}

bool DataOffer::Done() const {
  if (!sealed) return false;
  int n = Count();
  for (int i = 0; i < n; ++i) {
    if (!entries[i].done.load(std::memory_order_acquire)) return false;
  }
  return true;
}

namespace {

constexpr float kCard = 2.4_cm;
constexpr float kIconInset = 3_mm;
constexpr float kRingR = kCard * 0.62f;

struct Row {
  Str name;
  float progress;  // -1 indeterminate, else [0, 1]
};

Str ExtensionOf(StrView name) {
  auto dot = name.rfind('.');
  return dot == StrView::npos ? Str() : Str(name.substr(dot + 1));
}

}  // namespace

struct DataOfferToy : automat::ObjectToy {
  Vec<Row> rows;
  bool animating = false;
  float phase = 0;
  mutable std::unordered_map<Str, sk_sp<SkPicture>> icon_cache;

  DataOfferToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) { Pull(); }

  bool CenteredAtZero() const override { return true; }

  void Pull() {
    rows.clear();
    animating = false;
    if (auto offer = LockObject<DataOffer>()) {
      int n = offer->Count();
      for (int i = 0; i < n; ++i) {
        auto& e = offer->entries[i];
        bool done = e.done.load(std::memory_order_acquire);
        float progress;
        if (done) {
          progress = 1;
        } else {
          uint64_t total = e.bytes_total.load(std::memory_order_relaxed);
          uint64_t got = e.bytes_done.load(std::memory_order_relaxed);
          progress = total ? (float)((double)got / (double)total) : -1;
          animating = true;
        }
        rows.push_back({e.name, progress});
      }
      if (n == 0) animating = true;
    }
  }

  const sk_sp<SkPicture>& IconFor(const Str& name) const {
    Str ext = ExtensionOf(name);
    auto it = icon_cache.find(ext);
    if (it == icon_cache.end()) it = icon_cache.emplace(ext, IconForExtension(ext)).first;
    return it->second;
  }

  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(kCard, kCard), 1.5_mm).sk);
  }
  Optional<Rect> DrawBounds() const override {
    return Rect::MakeCenterZero(kCard, kCard).Outset(8_mm);
  }

  Tock Tick(time::Timer& timer) override {
    Pull();
    if (animating) {
      phase = (float)std::fmod(timer.NowSeconds() * 0.8, 1.0);
      return Tock::Drawing;
    }
    return Tock::Draw;
  }

  void DrawCard(SkCanvas& canvas, Vec2 c, const Row& row, float shade, bool front) const {
    uint32_t seed = Hash2(ID(), (uint32_t)std::lround(c.x * 1000));
    Rect card = Rect::MakeCenter(c, kCard, kCard);
    SkPath body = WonkyRoundRect(card, 1.2_mm, kWonk, seed);
    HandShadow(canvas, body, {kShadowDX, -kShadowDY}, kShadow, seed);
    canvas.drawPath(body, Fill(shade < 1 ? 0xffdcdcdc : kPaper));
    if (auto& icon = IconFor(row.name)) DrawIconIn(canvas, *icon, card.Inset(kIconInset));
    SketchyStroke(canvas, body, kInk, kStroke, seed, 2);
    if (front) DrawRing(canvas, c, row.progress, seed);
  }

  void DrawRing(SkCanvas& canvas, Vec2 c, float progress, uint32_t seed) const {
    SkRect oval = Rect::MakeCenter(c, kRingR * 2, kRingR * 2).sk;
    canvas.drawArc(oval, 0, 360, false, InkPaint(kGray, kStrokeBold));
    SkPaint arc = InkPaint(kGreen, kStrokeBold);
    if (progress < 0) {
      canvas.drawArc(oval, phase * 360.f, 90, false, arc);
    } else {
      canvas.drawArc(oval, -90, progress * 360.f, false, arc);
    }
  }

  void Draw(SkCanvas& canvas) const override {
    if (rows.empty()) {
      Rect card = Rect::MakeCenterZero(kCard, kCard);
      SketchyStroke(canvas, WonkyRoundRect(card, 1.2_mm, kWonk, ID()), kInkSoft, kStroke, ID(), 2);
      DrawRing(canvas, {0, 0}, -1, ID());
      return;
    }
    int shown = std::min<int>(rows.size(), 3);
    for (int i = shown - 1; i >= 1; --i) {
      DrawCard(canvas, Vec2(i * 2.2_mm, i * -1.6_mm), rows[i], 0.85f, false);
    }
    DrawCard(canvas, {0, 0}, rows[0], 1.f, true);
    DrawBetaStamp(canvas, {kCard / 2 - 0.58_mm, kCard / 2 - 0.875_mm}, 3.1_mm, -12.f,
                  Hash2(ID(), 12u));
    if (rows.size() > 3) {
      DrawText(canvas, f("+{}", rows.size() - 3), {kCard / 2 - 6_mm, -kCard / 2 + 1_mm}, 3_mm, kInk,
               true, ID());
    }
  }
};

std::unique_ptr<automat::ObjectToy> DataOffer::MakeToy(ui::Widget* parent) {
  return std::make_unique<DataOfferToy>(parent, *this);
}

}  // namespace automat::library
