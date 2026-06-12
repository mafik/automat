// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include "library_leptonica.hh"

#include <include/core/SkBlurTypes.h>
#include <include/core/SkCanvas.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorFilter.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkMaskFilter.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkPathUtils.h>
#include <include/core/SkPixmap.h>
#include <include/core/SkRRect.h>
#include <include/core/SkSamplingOptions.h>
#include <include/core/SkShader.h>
#include <include/core/SkSize.h>
#include <include/core/SkSpan.h>
#include <include/core/SkSurface.h>
#include <include/effects/SkDashPathEffect.h>
#include <include/effects/SkGradient.h>
#include <include/pathops/SkPathOps.h>
#include <leptonica/allheaders.h>
#include <rapidjson/rapidjson.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <queue>
#include <string>
#include <vector>

#include "automat.hh"  // vm
#include "color.hh"
#include "drag_action.hh"
#include "font.hh"
#include "library_window.hh"
#include "location.hh"
#include "path.hh"
#include "pointer.hh"
#include "root_widget.hh"
#include "svg.hh"
#include "text_widget.hh"
#include "ui_button.hh"
#include "ui_enum_knob_widget.hh"
#include "ui_leptonica.hh"
#include "ui_shape_widget.hh"
#include "ui_slop.hh"
#include "units.hh"
#include "widget.hh"

using namespace std::literals;

namespace automat::library {

namespace slop = ui::slop;

// Port icons, drawn white like the stock Next arrow; ConnectionWidget
// scales them onto the plug face.

std::unique_ptr<ui::Widget> MakePhotoPortIcon(ui::Widget* parent) {
  return ui::MakeShapeWidget(
      parent,
      PathFromSVG("M-10-8h20v2.4h-20zM-10 5.6h20v2.4h-20zM-10-8h2.4v16h-2.4zM7.6-8h2.4v16h-2.4z"
                  "M-6.5 5.6l3.8-6.5 2.6 3.4 1.8-1.9 3 5zM3.4-4.7l2.2 2.2 2.2-2.2-2.2-2.2z"),
      "#ffffff"_color);
}

std::unique_ptr<ui::Widget> MakeScalarPortIcon(ui::Widget* parent) {
  return ui::MakeShapeWidget(
      parent, PathFromSVG("M-4.6-9h3v18h-3zM1.6-9h3v18h-3zM-9-4.6h18v3h-18zM-9 1.6h18v3h-18z"),
      "#ffffff"_color);
}

std::unique_ptr<ui::Widget> MakeSwatchPortIcon(ui::Widget* parent) {
  return ui::MakeShapeWidget(
      parent, PathFromSVG("M-9-9h8v8h-8zM1-9h8v8h-8zM-9 1h8v8h-8zM1 1h8v8h-8z"), "#ffffff"_color);
}

// ============================================================================
// Shared leptonica palette & helpers
// ============================================================================

namespace {

constexpr SkColor4f kPlateTop = "#3b322c"_color4f;
constexpr SkColor4f kPlateBottom = "#211c19"_color4f;
constexpr SkColor kAmber = "#ef9f37"_color;
constexpr SkColor kAmberBright = "#ffd089"_color;
constexpr SkColor kSafelight = "#c43b1f"_color;
constexpr SkColor kPaper = "#f4efe4"_color;
constexpr SkColor kInk = "#241f1c"_color;

constexpr float kPxToMetric = 7_cm / static_cast<float>(LeptonicaImage::kDefaultW);

constexpr float kTitleTextPx = 40.f;
constexpr float kCreditTextPx = 19.f;
constexpr SkColor kLabelInk = "#554d3e"_color;

ui::Font& LabelFont() {
  static std::unique_ptr<ui::Font> font = ui::Font::MakeV2(ui::Font::GetHeavyData(), 4.5_mm);
  return *font;
}

ui::Font& TinyFont() {
  static std::unique_ptr<ui::Font> font = ui::Font::MakeV2(ui::Font::GetHeavyData(), 2.6_mm);
  return *font;
}

// The slop kit draws in pixels with +Y down; these canvases are metric with
// +Y up. SlopHere bridges them: anchor at a metric point, scale one slop
// pixel to kPxToMetric, flip Y. Inside the guard, draw in raw slop pixels.
struct SlopHere {
  SkCanvas& canvas;
  SlopHere(SkCanvas& c, SkPoint anchor_m) : canvas(c) {
    c.save();
    c.translate(anchor_m.fX, anchor_m.fY);
    c.scale(kPxToMetric, -kPxToMetric);
  }
  ~SlopHere() { canvas.restore(); }
};

// Name label centred on metric (cx, baseline_y); a non-empty fn_name adds a
// smaller credit line with the Leptonica function below it.
void SlopLabel(SkCanvas& canvas, std::string_view text, float cx, float baseline_y,
               SkColor color = kAmberBright, float size_px = 44.f, std::string_view fn_name = {}) {
  SlopHere g(canvas, {0, 0});
  float half = slop::TextWidth(text, size_px) * 0.5f;
  float base_x = cx / kPxToMetric - half;  // left edge of the centred run, in slop px
  float base_y = -baseline_y / kPxToMetric;
  float dx = size_px * 0.04f, dy = size_px * 0.05f;  // shadow offset tracks the lettering size
  constexpr uint32_t kSeed = 0x10C;
  slop::DrawText(canvas, text, {base_x + dx, base_y + dy}, size_px, 0xcc0a0806, true, kSeed);
  slop::DrawText(canvas, text, {base_x, base_y}, size_px, color, true, kSeed);
  if (!fn_name.empty()) {
    std::string credit(fn_name);
    credit += "()";
    float sub_size = size_px * 0.57f;  // clearly smaller than the name
    float sub_half = slop::TextWidth(credit, sub_size) * 0.5f;
    float sub_x = cx / kPxToMetric - sub_half;  // re-centre the shorter run on the same cx
    float sub_y = base_y + size_px * 0.86f;  // tuck just under the name baseline with a small gap
    slop::DrawText(canvas, credit, {sub_x, sub_y}, sub_size, "#a99a86"_color, true, kSeed);
  }
}

// Leptonica stores a pixel as a 32-bit word R<<24|G<<16|B<<8|A; on a
// little-endian host the bytes are [A,B,G,R], so byte-swapping yields Skia's
// kRGBA_8888. Alpha is honoured only when spp == 4, otherwise forced opaque.
sk_sp<SkImage> PixToSkImage(Pix* pix) {
  if (!pix) return nullptr;
  Pix* rgba = pix;
  bool owned = false;
  if (pixGetDepth(pix) != 32) {
    rgba = pixConvertTo32(pix);
    if (!rgba) return nullptr;
    owned = true;
  }
  int w = pixGetWidth(rgba);
  int h = pixGetHeight(rgba);
  l_int32 wpl = pixGetWpl(rgba);
  l_uint32* data = pixGetData(rgba);
  const bool use_alpha = pixGetSpp(rgba) == 4;
  std::vector<uint32_t> buffer(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    l_uint32* line = data + static_cast<size_t>(y) * wpl;
    uint32_t* dst = buffer.data() + static_cast<size_t>(y) * w;
    for (int x = 0; x < w; ++x) {
      dst[x] = use_alpha ? __builtin_bswap32(line[x]) : (__builtin_bswap32(line[x]) | 0xff000000u);
    }
  }
  auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType);
  SkPixmap pixmap(info, buffer.data(), static_cast<size_t>(w) * 4);
  auto image = SkImages::RasterFromPixmapCopy(pixmap);
  if (owned) pixDestroy(&rgba);
  return image;
}

Pix* MakeBlankPix(int w, int h, uint32_t fill) {
  Pix* p = pixCreate(w, h, 32);
  if (!p) return nullptr;
  pixSetAllArbitrary(p, fill);
  return p;
}

// Image sidecar files live next to automat_state.json (see persistence.cc::StatePath).
Path ImageDir() { return Path::ExecutablePath().Parent(); }

// Monotonic source of "imageN.bmp" names, kept ahead of any name seen during loading so freshly
// saved surfaces never clobber an existing sidecar.
std::atomic<int> g_image_index{0};

Str NextImageName() { return Str("image") + std::to_string(g_image_index.fetch_add(1)) + ".bmp"; }

void ObserveImageName(StrView name) {
  if (name.size() > 5 && name.starts_with("image")) {
    int n = atoi(name.data() + 5);
    int cur = g_image_index.load();
    while (n + 1 > cur && !g_image_index.compare_exchange_weak(cur, n + 1)) {
    }
  }
}

// Inverse of PixToSkImage: RGBA bytes byte-swapped into Leptonica words.
Pix* SkImageToPix(const sk_sp<SkImage>& img) {
  if (!img) return nullptr;
  int w = img->width(), h = img->height();
  if (w <= 0 || h <= 0) return nullptr;
  std::vector<uint32_t> buffer(static_cast<size_t>(w) * h);
  auto info = SkImageInfo::Make(w, h, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType);
  if (!img->readPixels(info, buffer.data(), static_cast<size_t>(w) * 4, 0, 0)) return nullptr;
  Pix* pix = pixCreate(w, h, 32);
  if (!pix) return nullptr;
  l_int32 wpl = pixGetWpl(pix);
  l_uint32* data = pixGetData(pix);
  for (int y = 0; y < h; ++y) {
    l_uint32* line = data + static_cast<size_t>(y) * wpl;
    uint32_t* s = buffer.data() + static_cast<size_t>(y) * w;
    for (int x = 0; x < w; ++x) line[x] = __builtin_bswap32(s[x]);
  }
  return pix;
}

// Stable signature of the image behind an ImageProvider: Skia mints a fresh
// uniqueID per raster snapshot, and the cached snapshot regenerates whenever
// the pixels change. Widgets fold this into their dirty checks to catch
// upstream edits. 0 means "no image".
uint32_t SourceImageId(const sk_sp<SkImage>& src) { return src ? src->uniqueID() : 0u; }

// Cheap enough to poll every frame: a non-dirty LeptonicaImage just hands
// back its cached snapshot.
uint32_t PhotoToolSourceId(PhotoTool& tool) {
  auto nested = tool.image->FindInterface();
  Object* owner = nested.Owner<Object>();
  ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
  return SourceImageId(ip ? ip.GetImage() : nullptr);
}

}  // namespace

// ============================================================================
// LeptonicaImage
// ============================================================================

LeptonicaImage::LeptonicaImage() {}

LeptonicaImage::LeptonicaImage(const LeptonicaImage& o) : Object(o), fill_pixel(o.fill_pixel) {
  auto lock = std::lock_guard(o.mutex);
  if (o.pix) pix = pixCopy(nullptr, o.pix);
  image_dirty = true;
}

LeptonicaImage::~LeptonicaImage() {
  if (pix) pixDestroy(&pix);
}

void LeptonicaImage::EnsurePixLocked() {
  if (!pix) {
    pix = MakeBlankPix(kDefaultW, kDefaultH, fill_pixel);
    image_dirty = true;
  }
}

sk_sp<SkImage> LeptonicaImage::EnsureImageLocked() {
  EnsurePixLocked();
  if (image_dirty) {
    cached_image = PixToSkImage(pix);
    image_dirty = false;
  }
  return cached_image;
}

void LeptonicaImage::Transform(const std::function<Pix*(Pix*)>& op) {
  auto lock = std::lock_guard(mutex);
  EnsurePixLocked();
  Pix* result = op(pix);
  if (result && result != pix) {
    pixDestroy(&pix);
    pix = result;
  }
  // The result is adopted AS-IS: native depth and colormap are the point (a Threshold leaves a
  // true 1-bpp image here). Display conversion happens only in the snapshot path.
  image_dirty = true;
  WakeToys();
}

void LeptonicaImage::Resize(int w, int h) {
  w = std::clamp(w, kMinSide, kMaxSide);
  h = std::clamp(h, kMinSide, kMaxSide);
  Transform([&](Pix* in) -> Pix* {
    Pix* dst = MakeBlankPix(w, h, fill_pixel);
    if (!dst) return nullptr;
    int cw = std::min(w, pixGetWidth(in));
    int ch = std::min(h, pixGetHeight(in));
    pixRasterop(dst, 0, 0, cw, ch, PIX_SRC, in, 0, 0);
    return dst;
  });
}

void LeptonicaImage::Dimensions(int& w, int& h) {
  auto lock = std::lock_guard(mutex);
  EnsurePixLocked();
  w = pix ? pixGetWidth(pix) : 0;
  h = pix ? pixGetHeight(pix) : 0;
}

Ptr<Object> LeptonicaImage::Clone() const { return MAKE_PTR(LeptonicaImage, *this); }

void LeptonicaImage::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  const_cast<LeptonicaImage*>(this)->EnsurePixLocked();
  if (image_filename.empty()) image_filename = NextImageName();
  // Persist the pixels into a human-readable .bmp next to automat_state.json. Leptonica's BMP I/O
  // is built in, so it works even though the PNG/JPEG codecs are disabled in this build.
  if (pix) {
    Path path = ImageDir() / image_filename;
    pixWrite(path.c_str(), pix, IFF_BMP);
  }
  writer.Key("image");
  writer.String(image_filename.data(), static_cast<rapidjson::SizeType>(image_filename.size()));
}

bool LeptonicaImage::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  if (key == "image") {
    Str fname;
    d.Get(fname, status);
    if (OK(status)) {
      ObserveImageName(fname);
      Path path = ImageDir() / fname;
      Pix* loaded = pixRead(path.c_str());
      if (loaded) {
        if (pixGetDepth(loaded) != 32) {
          Pix* p32 = pixConvertTo32(loaded);
          pixDestroy(&loaded);
          loaded = p32;
        }
        auto lock = std::lock_guard(mutex);
        if (pix) pixDestroy(&pix);
        pix = loaded;
        image_filename = fname;
        image_dirty = true;
        WakeToys();
      } else {
        ReportError(Str("Couldn't load image file: ") + Str(fname));
      }
    }
  } else if (key == "width") {
    int v = kDefaultW;
    d.Get(v, status);
    if (OK(status)) {
      int w, h;
      Dimensions(w, h);
      Resize(v, h ? h : kDefaultH);
    }
  } else if (key == "height") {
    int v = kDefaultH;
    d.Get(v, status);
    if (OK(status)) {
      int w, h;
      Dimensions(w, h);
      Resize(w ? w : kDefaultW, v);
    }
  } else {
    return false;
  }
  if (!OK(status)) ReportError(status.ToStr());
  return true;
}

void PhotoTool::Develop() {
  auto nested = image->FindInterface();
  Object* owner = nested.Owner<Object>();
  if (!owner) {
    ReportError("Connect me to an image first!");
    return;
  }
  float snapshot[kMaxParams];
  {
    auto lock = std::lock_guard(mutex);
    for (int i = 0; i < kMaxParams; ++i) snapshot[i] = params[i];
  }
  // The native fast path: a LeptonicaImage hands its Pix straight to the op - no SkImage
  // round-trip, no depth conversion. Any other ImageProvider is a read-only source.
  if (auto* li = dynamic_cast<LeptonicaImage*>(owner)) {
    li->Transform([this, &snapshot](Pix* in) { return ApplyOp(in, snapshot); });
    ClearOwnError();
  } else {
    ReportError("I can only develop onto a LeptonicaImage.");
  }
}

void PhotoTool::SerializeState(ObjectSerializer& writer) const {
  auto infos = ParamInfos();
  auto lock = std::lock_guard(mutex);
  for (size_t i = 0; i < infos.size(); ++i) {
    writer.Key(infos[i].name.data(), static_cast<rapidjson::SizeType>(infos[i].name.size()));
    writer.Double(params[i]);
  }
}

bool PhotoTool::DeserializeKey(ObjectDeserializer& d, StrView key) {
  auto infos = ParamInfos();
  for (size_t i = 0; i < infos.size(); ++i) {
    if (key == infos[i].name) {
      float v = 0;
      Status status;
      d.Get(v, status);
      if (OK(status)) {
        auto lock = std::lock_guard(mutex);
        params[i] = v;
      }
      WakeToys();
      return true;
    }
  }
  return false;
}

// ============================================================================
// Concrete tools
// ============================================================================

bool Threshold::DrivenLevel(float& out) const {
  if (auto* src = level_src->ObjectOrNull()) {
    auto txt = src->GetText();
    if (!txt.empty()) {
      char* end = nullptr;
      double v = strtod(txt.c_str(), &end);
      if (end != txt.c_str()) {
        out = std::clamp((float)v, 1.f, 254.f);
        return true;
      }
    }
  }
  return false;
}

Pix* Threshold::ApplyOp(Pix* in, const float* p) const {
  int lvl = std::clamp((int)p[0], 1, 254);
  float driven;
  if (DrivenLevel(driven)) lvl = (int)driven;  // the Level port overrides the hand-set value
  bool bfg;
  int meth;
  {
    auto lock = std::lock_guard(mutex);
    bfg = bright_fg;
    meth = method;
  }
  Pix* gray = pixConvertRGBToGray(in, 0.3f, 0.59f, 0.11f);
  if (!gray) return nullptr;
  Pix* bin = nullptr;
  if (meth == 1) {
    // OTSU: one global tile - the threshold is FOUND (the toy shows where the blade parked).
    Pix* pixd = nullptr;
    pixOtsuAdaptiveThreshold(gray, pixGetWidth(gray), pixGetHeight(gray), 0, 0, 0.f, nullptr,
                             &pixd);
    bin = pixd;
  } else if (meth == 2) {
    // SAUVOLA: locally adaptive - there is no single level.
    Pix* pixd = nullptr;
    pixSauvolaBinarizeTiled(gray, 25, 0.35f, 1, 1, nullptr, &pixd);
    bin = pixd;
  } else if (meth == 3) {
    // COMPS: search 80..200 for the level where the connected-component count stabilizes.
    Pix* pixd = nullptr;
    pixThresholdByConnComp(gray, nullptr, 80, 200, 6, 0.1f, 0.1f, nullptr, &pixd, 0);
    bin = pixd;
  } else {
    bin = pixThresholdToBinary(gray, lvl);
  }
  pixDestroy(&gray);
  if (bin && bfg) pixInvert(bin, bin);  // polarity: the BRIGHT side becomes ink
  return bin;                           // native 1 bpp - preview + surface upconvert
}

void Threshold::SerializeState(ObjectSerializer& writer) const {
  PhotoTool::SerializeState(writer);
  auto lock = std::lock_guard(mutex);
  writer.Key("bright_fg");
  writer.Bool(bright_fg);
  writer.Key("method");
  writer.Int(method);
}
bool Threshold::DeserializeKey(ObjectDeserializer& d, StrView key) {
  if (key == "method") {
    Status status;
    int v = 0;
    d.Get(v, status);
    if (OK(status)) {
      auto lock = std::lock_guard(mutex);
      method = std::clamp(v, 0, 3);
    }
    return true;
  }
  if (key == "bright_fg") {
    Status status;
    bool v = false;
    d.Get(v, status);
    if (OK(status)) {
      auto lock = std::lock_guard(mutex);
      bright_fg = v;
    }
    return true;
  }
  return PhotoTool::DeserializeKey(d, key);
}

void Threshold::DrawSymbol(SkCanvas& canvas) const {
  constexpr float r = 3.4_mm;
  SkRect oval = Rect::MakeCircleR(r).sk;
  SkPaint white;
  white.setAntiAlias(true);
  white.setColor("#ffffff"_color);
  SkPaint black;
  black.setAntiAlias(true);
  black.setColor(kInk);
  canvas.drawArc(oval, 90, 180, true, white);
  canvas.drawArc(oval, 270, 180, true, black);
  SkPaint rim;
  rim.setAntiAlias(true);
  rim.setStyle(SkPaint::kStroke_Style);
  rim.setStrokeWidth(0.4_mm);
  rim.setColor(kInk);
  canvas.drawCircle(0, 0, r, rim);
}

Span<const PhotoTool::ParamInfo> Threshold::ParamInfos() const {
  static constexpr ParamInfo infos[] = {{"Level"sv, 1.0f, 254.0f, ""sv}};
  return Span<const ParamInfo>(infos);
}
// ============================================================================
// Widgets
// ============================================================================

struct PhotoToolWidget;

// A knob bound to one continuous tool parameter, quantised into discrete steps.
struct ParamKnob : ui::EnumKnobWidget {
  PhotoToolWidget& tool_widget;
  int index;
  PhotoTool::ParamInfo info;
  int steps;

  static int StepsFor(const PhotoTool::ParamInfo& info) {
    if (!info.options.empty()) return (int)info.options.size();
    if (info.integer) return std::clamp((int)(info.max - info.min) + 1, 2, 100);
    return 41;
  }

  ParamKnob(ui::Widget* parent, PhotoToolWidget& tw, int index, PhotoTool::ParamInfo info);
  float Value(int step) const;
  int Step(float v) const;
  int KnobGet() const override;
  void KnobSet(int step) override;
  void DrawKnobSymbol(SkCanvas& canvas, int step) const override;
};

struct PhotoToolWidget : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_param_hash = 0;
  uint32_t preview_source_id = 0;  // uniqueID of the source image the cache was built from
  bool preview_dirty = true;

  float plate_w = 3.8_cm;
  float plate_h = 5.2_cm;
  float preview_h = 6.0_cm;
  Rect preview_rect;
  float rail_y = 0;

  constexpr static float kCorner = 3_mm;
  constexpr static float kCell = 1.5_cm;
  constexpr static float kRowH = 1.75_cm;
  constexpr static float kRailH = 1.8_cm;

  std::unique_ptr<ui::slop::RunButton> glass;

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  Vec<std::unique_ptr<ParamKnob>> knobs;
  Vec<std::string> param_names;
  Vec<Vec2> knob_pos;

  std::string label;
  std::string explanation;
  int expl_lines = 1;  // wrapped explanation line count (drives the plate's bottom budget)

  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  PhotoToolWidget(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto tool = LockObject<PhotoTool>()) tool->develop->ScheduleRun();
    });

    if (auto tool = LockTool()) {
      label = std::string(tool->Label());
      explanation = std::string(tool->Explanation());
      auto infos = tool->ParamInfos();
      int n = (int)infos.size();
      int cols = std::clamp(n, 1, 3);
      int rows = n == 0 ? 0 : (n + cols - 1) / cols;

      plate_w = std::max<float>(3.8_cm, cols * kCell + 1.0_cm);
      preview_h = std::max<float>(6.0_cm, 9.0_cm - rows * 0.8_cm);

      // Estimate the wrap without measuring: ui::GetFont() is unsafe during the
      // render-tree build. 0.42 * fontsize approximates the average glyph advance.
      float avg_glyph =
          2.2_mm;  // empirical avg advance of ui::GetFont(); slightly high = safe over-budget
      int chars_per_line = std::max(1, (int)((plate_w - 6_mm) / avg_glyph));
      expl_lines =
          std::clamp((int)((explanation.size() + chars_per_line - 1) / chars_per_line), 1, 8);
      // The explanation column plus clearance for the run disc straddling the
      // bottom border.
      float explanation_h = expl_lines * 3.2_mm + 0.9_cm;
      float rail_h = kRailH + std::max(0, rows - 1) * kRowH;
      plate_h = 0.4_cm + preview_h + rail_h + explanation_h + 0.3_cm;

      Rect plate = Rect::MakeCenterZero(plate_w, plate_h);
      float preview_top = plate.top - 0.4_cm;


      preview_rect =
          Rect::MakeCenter({0, preview_top - preview_h / 2}, plate_w - 6_mm, preview_h - 6_mm);
      rail_y = preview_top - preview_h - 2_mm;

      for (int i = 0; i < n; ++i) {
        int col = i % cols;
        int row = i / cols;
        int row_n = std::min(cols, n - row * cols);
        float x = (col - (row_n - 1) / 2.0f) * kCell;
        float y = rail_y - row * kRowH;

        auto knob = std::make_unique<ParamKnob>(this, *this, i, infos[i]);
        knob->local_to_parent = SkM44::Translate(x, y);
        knobs.push_back(std::move(knob));
        param_names.push_back(std::string(infos[i].name));
        knob_pos.push_back({x, y});
      }
    }
  }

  bool CenteredAtZero() const override { return true; }

  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(plate_w, plate_h), kCorner).sk);
  }

  uint32_t ComputeParamHash() const {
    uint32_t hash = 2166136261u;
    if (auto tool = LockTool()) {
      auto lock = std::lock_guard(tool->mutex);
      for (int i = 0; i < PhotoTool::kMaxParams; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &tool->params[i], sizeof(float));
        hash ^= bits;
        hash *= 16777619u;
      }
    }
    return hash;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    if (!tool) {
      cached_preview = nullptr;
      return;
    }

    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider image_prov = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = image_prov ? image_prov.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) {
      cached_preview = nullptr;
      return;
    }

    int src_w = source->width();
    int src_h = source->height();
    if (src_w <= 0 || src_h <= 0) {
      cached_preview = nullptr;
      return;
    }
    float scale = std::min(200.0f / src_w, 200.0f / src_h);
    int downscaled_w = std::max(1, (int)(src_w * scale));
    int downscaled_h = std::max(1, (int)(src_h * scale));

    auto scaled_surface = SkSurfaces::Raster(SkImageInfo::Make(
        downscaled_w, downscaled_h, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!scaled_surface) {
      cached_preview = nullptr;
      return;
    }

    auto* scaled_canvas = scaled_surface->getCanvas();
    scaled_canvas->drawImageRect(source, SkRect::Make(SkISize{src_w, src_h}),
                                 SkRect::Make(SkISize{downscaled_w, downscaled_h}),
                                 SkSamplingOptions(SkFilterMode::kLinear), nullptr,
                                 SkCanvas::kFast_SrcRectConstraint);

    sk_sp<SkImage> downscaled_image = scaled_surface->makeImageSnapshot();
    if (!downscaled_image) {
      cached_preview = nullptr;
      return;
    }

    Pix* pix = SkImageToPix(downscaled_image);
    if (!pix) {
      cached_preview = nullptr;
      return;
    }

    float snapshot[PhotoTool::kMaxParams];
    {
      auto lock = std::lock_guard(tool->mutex);
      for (int i = 0; i < PhotoTool::kMaxParams; ++i) {
        snapshot[i] = tool->params[i];
      }
    }

    Pix* result = tool->ApplyOp(pix, snapshot);
    pixDestroy(&pix);

    if (!result) {
      cached_preview = nullptr;
      return;
    }

    Pix* result_32 = result;
    if (pixGetDepth(result) != 32) {
      result_32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!result_32) {
        cached_preview = nullptr;
        return;
      }
    }

    cached_preview = PixToSkImage(result_32);
    if (result_32 != result) pixDestroy(&result_32);

    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer& timer) override {
    bool has_source = false;
    if (auto tool = LockTool()) {
      std::string l(tool->Label());
      if (l != label) label = l;

      // Recompute when our params change OR when the connected source image changes.
      uint32_t hash = ComputeParamHash();
      if (hash != preview_param_hash) {
        preview_param_hash = hash;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*tool) != preview_source_id) preview_dirty = true;

      if (preview_dirty) {
        RecomputePreview();
      }
      has_source = preview_source_id != 0;
    }

    // With a source connected keep polling so upstream edits are picked up; with
    // none, idle instead of re-recording the static shelf previews every frame.
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    Rect plate = Rect::MakeCenterZero(plate_w, plate_h);

    SkRRect rrect = RRect::MakeSimple(plate, kCorner).sk;
    SkPaint body;
    body.setAntiAlias(true);
    SkPoint pts[2] = {{0, plate.top}, {0, plate.bottom}};
    SkColor4f cols[2] = {kPlateTop, kPlateBottom};
    body.setShader(SkShaders::LinearGradient(
        pts, SkGradient{SkGradient::Colors{cols, SkTileMode::kClamp}, {}}));
    canvas.drawRRect(rrect, body);

    SkPaint hole;
    hole.setAntiAlias(true);
    hole.setColor("#15110f"_color);
    float hole_y = plate.top - 3.5_mm;
    int n_holes = std::max(3, std::min(5, (int)(plate_w / 0.6_cm)));
    float hole_step = plate_w / (n_holes + 1);
    for (int i = 0; i < n_holes; ++i) {
      float hx = plate.left + hole_step * (i + 1);
      canvas.drawRRect(RRect::MakeSimple(Rect::MakeCenter({hx, hole_y}, 2.2_mm, 2.8_mm), 0.6_mm).sk,
                       hole);
    }

    if (cached_preview) {
      canvas.save();
      SkRect src = SkRect::Make(cached_preview->dimensions());
      SkMatrix m = SkMatrix::RectToRect(src, preview_rect.sk, SkMatrix::kFill_ScaleToFit);
      m.preTranslate(0, cached_preview->height() / 2.f);
      m.preScale(1, -1);
      m.preTranslate(0, -cached_preview->height() / 2.f);
      canvas.concat(m);
      canvas.drawImage(cached_preview, 0, 0, SkSamplingOptions(), nullptr);
      canvas.restore();
    } else {
      SkPaint placeholder;
      placeholder.setAntiAlias(true);
      placeholder.setColor("#4a3f35"_color);
      canvas.drawRect(preview_rect.sk, placeholder);
      SkPaint text;
      text.setAntiAlias(true);
      text.setColor("#999999"_color);
      auto& font = ui::GetFont();
      canvas.save();
      canvas.translate(-font.MeasureText("No photo") / 2, preview_rect.CenterY() + 1_mm);
      font.DrawText(canvas, "No photo", text);
      canvas.restore();
    }

    DrawChildren(canvas);

    auto& small = ui::GetFont();
    SkPaint plabel;
    plabel.setAntiAlias(true);
    plabel.setColor("#d8c8b0"_color);
    for (size_t i = 0; i < param_names.size(); ++i) {
      canvas.save();
      canvas.translate(knob_pos[i].x - small.MeasureText(param_names[i]) / 2,
                       knob_pos[i].y - 0.66_cm);
      small.DrawText(canvas, param_names[i], plabel);
      canvas.restore();
    }

    SkPaint label_paint;
    label_paint.setAntiAlias(true);
    label_paint.setColor(kAmber);
    canvas.save();
    canvas.translate(-LabelFont().MeasureText(label) / 2, plate.top - 1.0_cm);
    LabelFont().DrawText(canvas, label, label_paint);
    canvas.restore();

    SkPaint exp_paint;
    exp_paint.setAntiAlias(true);
    exp_paint.setColor("#bdae9b"_color);
    float max_w = plate_w - 6_mm;
    float y = plate.bottom + 0.75_cm + (expl_lines - 1) * 3.2_mm;

    std::string line, word;
    auto flush_line = [&]() {
      if (line.empty()) return;
      canvas.save();
      canvas.translate(-small.MeasureText(line) / 2, y);
      small.DrawText(canvas, line, exp_paint);
      canvas.restore();
      y -= 3.2_mm;
      line.clear();
    };

    size_t pos = 0;
    while (pos <= explanation.size()) {
      if (pos == explanation.size() || explanation[pos] == ' ') {
        std::string candidate = line.empty() ? word : line + " " + word;
        if (small.MeasureText(candidate) > max_w && !line.empty()) {
          flush_line();
          line = word;
        } else {
          line = candidate;
        }
        word.clear();
        if (pos == explanation.size()) break;
      } else {
        word += explanation[pos];
      }
      ++pos;
    }
    flush_line();
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
    for (auto& k : knobs) children.push_back(k.get());
  }
};



ParamKnob::ParamKnob(ui::Widget* parent, PhotoToolWidget& tw, int index, PhotoTool::ParamInfo info)
    : ui::EnumKnobWidget(parent, StepsFor(info)),
      tool_widget(tw),
      index(index),
      info(info),
      steps(StepsFor(info)) {}

float ParamKnob::Value(int step) const {
  if (!info.options.empty()) return info.min + step;  // enum: value is the option index
  float v = info.min + (info.max - info.min) * step / (float)(steps - 1);
  return info.integer ? std::round(v) : v;
}

int ParamKnob::Step(float v) const {
  if (!info.options.empty()) return std::clamp((int)std::lround(v - info.min), 0, steps - 1);
  return std::clamp((int)std::lround((v - info.min) / (info.max - info.min) * (steps - 1)), 0,
                    steps - 1);
}

int ParamKnob::KnobGet() const {
  if (auto tool = tool_widget.LockTool()) {
    auto lock = std::lock_guard(tool->mutex);
    return Step(tool->params[index]);
  }
  return 0;
}

void ParamKnob::KnobSet(int step) {
  if (auto tool = tool_widget.LockTool()) {
    {
      auto lock = std::lock_guard(tool->mutex);
      tool->params[index] = Value(step);
    }
    tool->WakeToys();
  }
}

void ParamKnob::DrawKnobSymbol(SkCanvas& canvas, int step) const {
  auto& font = ui::GetFont();
  SkPaint t;
  t.setAntiAlias(true);
  t.setColor(kInk);
  char buf[24];
  std::string s;
  if (!info.options.empty()) {
    s = std::string(info.options[std::clamp(step, 0, (int)info.options.size() - 1)]);
  } else {
    float v = Value(step);
    if (info.integer)
      snprintf(buf, sizeof(buf), "%d", (int)std::lround(v));
    else
      snprintf(buf, sizeof(buf), "%.2g", v);
    s = buf;
  }
  float w = font.MeasureText(s);
  float sc = std::min(1.0f, 6.2_mm / std::max(0.0001f, w));
  canvas.save();
  canvas.scale(sc, sc);
  canvas.translate(-w / 2, -font.letter_height / 2);
  font.DrawText(canvas, s, t);
  canvas.restore();
}

// Displays the Pix at 1 source px == kPxToMetric.

struct LeptonicaImageWidget : ObjectToy {
  sk_sp<SkImage> image;
  int px_w = LeptonicaImage::kDefaultW;
  int px_h = LeptonicaImage::kDefaultH;
  bool translucent = false;  // image carries real alpha -> animate the backdrop
  float backdrop_phase = 0;  // 0..1 rotation of the HSV wheel

  Ptr<LeptonicaImage> LockImage() const { return LockObject<LeptonicaImage>(); }

  LeptonicaImageWidget(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {}

  bool CenteredAtZero() const override { return true; }

  float DispW() const { return px_w * kPxToMetric; }
  float DispH() const { return px_h * kPxToMetric; }

  Rect PaperRect() const { return Rect::MakeCenterZero(DispW(), DispH()); }

  SkPath Shape() const override { return SkPath::Rect(PaperRect().sk); }

  Optional<Rect> TextureBounds() const override {
    Rect p = PaperRect();
    return Rect(p.left - 1_mm, p.bottom - 7_mm, p.right + 1.5_mm, p.top + 1_mm);
  }

  animation::Phase Tick(time::Timer& timer) override {
    if (auto surface = LockImage()) {
      auto lock = std::lock_guard(surface->mutex);
      surface->EnsurePixLocked();
      int nw = surface->pix ? pixGetWidth(surface->pix) : LeptonicaImage::kDefaultW;
      int nh = surface->pix ? pixGetHeight(surface->pix) : LeptonicaImage::kDefaultH;
      if (nw != px_w || nh != px_h) {
        px_w = nw;
        px_h = nh;
      }
      image = surface->EnsureImageLocked();
      translucent = surface->pix && pixGetSpp(surface->pix) == 4;
    }
    if (translucent) {
      backdrop_phase = (float)std::fmod(timer.NowSeconds() * 0.06, 1.0);
      return animation::Animating;
    }
    return animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    Rect sheet = PaperRect();
    SkPaint shadow;
    shadow.setAntiAlias(true);
    shadow.setColor(SkColorSetARGB(80, 0, 0, 0));
    canvas.drawRect(sheet.MoveBy({0.6_mm, -0.6_mm}).sk, shadow);

    if (translucent) {
      // The backdrop behind transparent pixels: a slowly rotating HSV wheel.
      static constexpr SkColor4f kWheel[7] = {{1, 0, 0, 1}, {1, 1, 0, 1}, {0, 1, 0, 1},
                                              {0, 1, 1, 1}, {0, 0, 1, 1}, {1, 0, 1, 1},
                                              {1, 0, 0, 1}};
      canvas.save();
      canvas.clipRect(sheet.sk);
      canvas.rotate(backdrop_phase * 360.f);
      SkPaint wheel;
      wheel.setAntiAlias(true);
      wheel.setShader(SkShaders::SweepGradient(
          {0, 0}, SkGradient{SkGradient::Colors{kWheel, SkTileMode::kClamp}, {}}));
      float r = std::hypot(sheet.Width(), sheet.Height());
      canvas.drawRect(SkRect::MakeLTRB(-r, -r, r, r), wheel);
      canvas.restore();
    } else if (!image) {
      SkPaint paper_paint;
      paper_paint.setAntiAlias(true);
      paper_paint.setColor(kPaper);
      canvas.drawRect(sheet.sk, paper_paint);
    }

    if (image) {
      canvas.save();
      SkRect src = SkRect::Make(image->dimensions());
      SkMatrix m = SkMatrix::RectToRect(src, sheet.sk, SkMatrix::kFill_ScaleToFit);
      m.preTranslate(0, image->height() / 2.f);
      m.preScale(1, -1);
      m.preTranslate(0, -image->height() / 2.f);
      canvas.concat(m);
      canvas.drawImage(image, 0, 0, SkSamplingOptions(), nullptr);
      canvas.restore();
    }

    auto& font = ui::GetFont();
    SkPaint cap;
    cap.setAntiAlias(true);
    cap.setColor("#8a7f6c"_color);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d x %d", px_w, px_h);
    canvas.save();
    canvas.translate(-font.MeasureText(buf) / 2, sheet.bottom - 4_mm);
    font.DrawText(canvas, buf, cap);
    canvas.restore();

    DrawChildren(canvas);
  }
};

struct ShelfButton : ui::Widget {
  Ptr<Object> proto;
  Widget* proto_widget = nullptr;

  ShelfButton(ui::Widget* parent, Ptr<Object> proto)
      : ui::Widget(parent), proto(std::move(proto)) {}

  void Init() { proto_widget = &ToyStore().FindOrMake(*proto, this); }

  StrView Name() const override { return "ShelfButton"; }
  SkPath Shape() const override { return proto_widget->Shape(); }
  RRect CoarseBounds() const override { return proto_widget->CoarseBounds(); }
  Optional<Rect> TextureBounds() const override { return std::nullopt; }
  void FillChildren(Vec<Widget*>& children) override { children.push_back(proto_widget); }
  bool AllowChildPointerEvents(Widget&) const override { return false; }

  void PointerOver(ui::Pointer& p) override { hand_icon.emplace(p, ui::Pointer::kIconHand); }
  void PointerLeave(ui::Pointer&) override { hand_icon.reset(); }
  Optional<ui::Pointer::IconOverride> hand_icon;

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn != ui::PointerButton::Left) return nullptr;
    auto obj = proto->Clone();
    p.root_widget.toys.FindOrMake(*obj, this);
    auto loc = MAKE_PTR(Location, vm.root_location);
    loc->InsertHere(std::move(obj));
    return std::make_unique<DragLocationAction>(p, std::move(loc));
  }
};

struct LeptonicaShelfWidget : ObjectToy {
  // The curated tool set; each group is a framed row and the objects' own
  // widgets are the icons.
  struct GroupSpec {
    const char* label;
    SkColor accent;
    std::initializer_list<const char*> names;
  };
  static constexpr float kCell = 3.2_cm;
  static constexpr float kPad = 0.3_cm;        // frame padding around a group's cells
  static constexpr float kGroupGap = 0.55_cm;  // between group frames in a row
  static constexpr float kRowGap = 0.95_cm;    // between rows (room for the next row's caption)
  static constexpr float kHeader = 2.9_cm;
  static constexpr float kMargin = 0.85_cm;

  struct PlacedGroup {
    const char* label;
    SkColor accent;
    Rect frame;  // metric, y-up
    int first_button, count;
  };

  Vec<std::unique_ptr<ShelfButton>> buttons;
  Vec<PlacedGroup> groups;
  float sheet_w = 20_cm;
  float sheet_h = 16_cm;

  LeptonicaShelfWidget(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    static const GroupSpec kRow1[] = {
        {"PAPER", slop::kYellow, {"LeptonicaImage", "Generate"}},
        {"LOOK", slop::kCyan, {"Tone", "Color", "Channel", "Flatten", "Fade"}},
        {"NEIGHBORS", slop::kPurple, {"Convolve", "Blend"}},
    };
    static const GroupSpec kRow2[] = {
        {"PALETTE", slop::kRose, {"Quantize", "Posterize", "Dither"}},
        {"SHAPE", slop::kGreen, {"Geometry", "Warp", "Crop", "Deskew", "Reduce"}},
    };
    static const GroupSpec kRow3[] = {
        {"MEASURE", slop::kBlue, {"Find Level", "Count", "Measure"}},
        {"INK", slop::kOrange, {"Threshold", "Select", "Morphology", "Seedfill"}},
    };
    static const std::pair<const GroupSpec*, int> kRows[] = {{kRow1, 3}, {kRow2, 2}, {kRow3, 2}};

    float row_w[3] = {};
    int nrows = 3;
    for (int r = 0; r < nrows; ++r) {
      float w = 0;
      for (int g = 0; g < kRows[r].second; ++g) {
        const GroupSpec& gs = kRows[r].first[g];
        w += (float)gs.names.size() * kCell + 2 * kPad;
      }
      w += kGroupGap * (kRows[r].second - 1);
      row_w[r] = w;
    }
    float max_w = std::max({row_w[0], row_w[1], row_w[2]});
    sheet_w = max_w + 2 * kMargin;
    float row_h = kCell + 2 * kPad;
    sheet_h = kHeader + nrows * row_h + (nrows - 1) * kRowGap + kMargin;

    Rect sheet = Rect::MakeCenterZero(sheet_w, sheet_h);
    float y = sheet.top - kHeader;
    for (int r = 0; r < nrows; ++r) {
      float x = sheet.left + kMargin + (max_w - row_w[r]) * 0.5f;
      for (int g = 0; g < kRows[r].second; ++g) {
        const GroupSpec& gs = kRows[r].first[g];
        float gw = (float)gs.names.size() * kCell + 2 * kPad;
        PlacedGroup pg{gs.label, gs.accent, Rect(x, y - row_h, x + gw, y), (int)buttons.size(), 0};
        int i = 0;
        for (const char* name : gs.names) {
          auto* proto = prototypes ? prototypes->Find(name) : nullptr;
          if (!proto) {
            fprintf(stderr, "LeptonicaShelf: no prototype named \"%s\"\n", name);
            continue;
          }
          buttons.emplace_back(std::make_unique<ShelfButton>(this, proto->Clone()));
          buttons.back()->Init();
          float cx = x + kPad + kCell * (i + 0.5f);
          float cy = y - row_h * 0.5f;
          Rect src = buttons.back()->CoarseBounds().rect;
          Rect dst = Rect::MakeCenter({cx, cy}, kCell * 0.88f, kCell * 0.88f);
          buttons.back()->local_to_parent =
              SkM44(SkMatrix::RectToRect(src.sk, dst.sk, SkMatrix::kCenter_ScaleToFit));
          ++pg.count;
          ++i;
        }
        groups.push_back(pg);
        x += gw + kGroupGap;
      }
      y -= row_h + kRowGap;
    }
  }

  bool CenteredAtZero() const override { return true; }

  SkPath Shape() const override {
    return SkPath::RRect(RRect::MakeSimple(Rect::MakeCenterZero(sheet_w, sheet_h), 4_mm).sk);
  }

  // The SLOP stamp overhangs the top-right corner; without this it gets clipped at the sheet.
  Optional<Rect> TextureBounds() const override {
    return Shape().getBounds().makeOutset(2_cm, 2_cm);
  }

  void Draw(SkCanvas& canvas) const override {
    Rect sheet = Rect::MakeCenterZero(sheet_w, sheet_h);
    SkRRect rr = RRect::MakeSimple(sheet, 4_mm).sk;
    SkPaint bg;
    bg.setAntiAlias(true);
    SkPoint pts[2] = {{0, sheet.top}, {0, sheet.bottom}};
    SkColor4f cgrad[2] = {"#2a2420"_color4f, "#181513"_color4f};
    bg.setShader(SkShaders::LinearGradient(
        pts, SkGradient{SkGradient::Colors{cgrad, SkTileMode::kClamp}, {}}));
    canvas.drawRRect(rr, bg);
    SkPaint border;
    border.setAntiAlias(true);
    border.setStyle(SkPaint::kStroke_Style);
    border.setStrokeWidth(1_mm);
    border.setColor(kAmber);
    canvas.drawRRect(rr, border);

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float mx, float my) { return SkPoint{mx / kPxToMetric, -my / kPxToMetric}; };
      auto PX = [&](float m) { return m / kPxToMetric; };

      const char* heading = "LEPTONICA";
      float hpx = slop::TextWidth(heading, 56.f);
      SkPoint hb = P(-hpx * kPxToMetric * 0.5f, sheet.top - 1.75_cm);
      slop::DrawText(canvas, heading, hb, 56.f, kAmberBright, true, 0x1E);
      canvas.drawPath(slop::WobbleLine({hb.fX - 6, hb.fY + 16}, {hb.fX + hpx + 6, hb.fY + 16},
                                       slop::kWonk, slop::kSeg, 0x1F),
                      slop::InkPaint(kAmber, slop::kStrokeBold));

      for (auto& pg : groups) {
        SkRect fr = SkRect::MakeLTRB(pg.frame.left / kPxToMetric, -pg.frame.top / kPxToMetric,
                                     pg.frame.right / kPxToMetric, -pg.frame.bottom / kPxToMetric);
        SkPath frame = slop::WonkyRoundRect(fr, 10.f, slop::kWonk * 0.7f,
                                            slop::Hash2(0x70, (uint32_t)(intptr_t)pg.label));
        slop::SketchyStroke(canvas, frame, pg.accent, slop::kStroke,
                            slop::Hash2(0x71, (uint32_t)(intptr_t)pg.label), 1);
        float fs = 15.f;
        float lw = slop::TextWidth(pg.label, fs);
        SkRect tab = SkRect::MakeXYWH(fr.fLeft + 10.f, fr.fTop - fs * 0.78f, lw + 12.f, fs * 1.35f);
        SkPath tabp = slop::WonkyRoundRect(tab, 4.f, slop::kWonk * 0.5f,
                                           slop::Hash2(0x72, (uint32_t)(intptr_t)pg.label));
        slop::FillPath(canvas, tabp, "#241f1b"_color);
        slop::SketchyStroke(canvas, tabp, pg.accent, slop::kStrokeHair,
                            slop::Hash2(0x73, (uint32_t)(intptr_t)pg.label), 1);
        slop::DrawText(canvas, pg.label, {tab.fLeft + 6.f, tab.fBottom - fs * 0.28f}, fs, pg.accent,
                       false, 0);
      }

      slop::DrawSparkle(canvas, P(sheet.right - 3.6_cm, sheet.top - 1.0_cm), PX(5_mm),
                        slop::kYellow, 0x41);
      slop::DrawSparkle(canvas, P(-0.16f * sheet_w, sheet.top - 1.5_cm), PX(3.5_mm), slop::kRose,
                        0x42);
    }

    DrawChildren(canvas);

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float mx, float my) { return SkPoint{mx / kPxToMetric, -my / kPxToMetric}; };
      slop::DrawSlopStamp(canvas, P(sheet.right - 0.6_cm, sheet.top - 0.6_cm), 2.2_cm / kPxToMetric,
                          -15.f, 0xA1, "SLOP");
    }
  }

  void FillChildren(Vec<Widget*>& children) override {
    for (auto& b : buttons) children.push_back(b.get());
  }
};

// ============================================================================
// MakeToy
// ============================================================================

std::unique_ptr<ObjectToy> LeptonicaImage::MakeToy(ui::Widget* parent) {
  return std::make_unique<LeptonicaImageWidget>(parent, *this);
}

// Draws img centred in area (metric, y-up) preserving aspect ratio, with a
// thin keyline.
static void DrawPreviewFitted(SkCanvas& canvas, const sk_sp<SkImage>& img, const Rect& area) {
  if (!img || img->width() <= 0 || img->height() <= 0) return;
  float s = std::min(area.Width() / img->width(), area.Height() / img->height());
  Rect fit =
      Rect::MakeCenter({area.CenterX(), area.CenterY()}, img->width() * s, img->height() * s);
  canvas.save();
  SkRect src = SkRect::Make(img->dimensions());
  SkMatrix m = SkMatrix::RectToRect(src, fit.sk, SkMatrix::kFill_ScaleToFit);
  m.preTranslate(0, img->height() / 2.f);
  m.preScale(1, -1);
  m.preTranslate(0, -img->height() / 2.f);
  canvas.concat(m);
  // Linear, not nearest: nearest-downscale DELETES whole columns/rows, which silently erases
  // sparse 1-px features (e.g. an HMT match mask). Linear keeps them as faint-but-present.
  canvas.drawImage(img, 0, 0, SkSamplingOptions(SkFilterMode::kLinear), nullptr);
  canvas.restore();
  SkPaint key;
  key.setAntiAlias(true);
  key.setStyle(SkPaint::kStroke_Style);
  key.setStrokeWidth(0.3_mm);
  key.setColor("#3a342c"_color);
  canvas.drawRect(fit.sk, key);
}

// Drawn just outside the displayed image's lower-left frame edge (px space;
// call inside SlopHere). Lower-left is the output-data position.
static void DrawDepthChipPx(SkCanvas& canvas, const SkRect& box_px, const sk_sp<SkImage>& img,
                            int depth, bool has_cmap, uint32_t seed) {
  if (depth <= 0 || !img || img->width() <= 0 || img->height() <= 0) return;
  float s = std::min(box_px.width() / img->width(), box_px.height() / img->height());
  float fw = img->width() * s, fh = img->height() * s;
  SkRect fit = SkRect::MakeXYWH(box_px.centerX() - fw / 2, box_px.centerY() - fh / 2, fw, fh);
  SkRect r = SkRect::MakeLTRB(fit.fLeft + 4.f - 54.f, fit.fBottom - 30.f, fit.fLeft + 4.f,
                              fit.fBottom - 6.f);
  if (r.fLeft < box_px.fLeft + 2.f) r.offset(box_px.fLeft + 2.f - r.fLeft, 0.f);
  ui::leptonica::DrawDepthChip(canvas, r, depth, has_cmap, slop::State::Default, seed);
}

// ============================================================================
// Threshold
// ============================================================================
struct ThresholdToy;

struct ThresholdBladeDrag : Action {
  TrackedPtr<ThresholdToy> widget;
  ThresholdBladeDrag(ui::Pointer& p, ThresholdToy& w);
  ~ThresholdBladeDrag();
  void Update() override;
};

struct ThresholdPolarityPoke : Action {
  ThresholdPolarityPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct ThresholdToy : ObjectToy {
  sk_sp<SkImage> cached_preview;  // the live 1-bpp stencil (the result of the cut)
  uint32_t preview_param_hash = 0;
  uint32_t driven_hash = 0;
  bool driven = false;  // the Level port is connected - the blade is a readout
  bool bright_fg = false;
  int method = 0;
  int found_level = -1;  // OTSU's found threshold (proxy-measured), -1 = none
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;
  std::vector<uint32_t> histogram = std::vector<uint32_t>(256, 0);  // of the SOURCE brightness
  float max_log_count = 1.f;

  static constexpr int kSkyBins = 22;
  float skyline[kSkyBins] = {};  // normalised (0..1) bar heights = the silhouette's top edge

  float level = 128.f;  // mirror of params[0]
  bool dragging = false;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string label, fn_credit;

  constexpr static float kHalfW = 2.9_cm;
  constexpr static float kBaseY = 1.5_cm;   // histogram baseline; bars rise above it
  constexpr static float kHistH = 1.25_cm;  // tallest bar

  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  ThresholdToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto tool = LockObject<PhotoTool>()) tool->develop->ScheduleRun();
    });
    if (auto tool = LockTool()) {
      label = std::string(tool->Label());
      fn_credit = std::string(tool->LeptonicaFn());
      auto lock = std::lock_guard(tool->mutex);
      level = tool->params[0];
    }
  }

  bool CenteredAtZero() const override { return true; }

  // The clip/texture region is deliberately LARGER than the silhouette: the implicit clip defaults
  // to Shape().getBounds(), which would cut off anything outside the outline. Decorations may
  // overhang Shape() if they stay inside THIS rect — but anything INTERACTIVE must be unioned into
  // Shape() itself: the pointer path (FillPath, pointer.cc) only reaches a widget where
  // Shape().contains() the point, so a control outside the outline is dead to the mouse.
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.4_cm, CardBottomM() - 0.4_cm, kHalfW + 1.2_cm, CardTopM() + 0.5_cm);
  }

  float HistXL() const { return -kHalfW + 0.5_cm; }
  float HistXR() const { return kHalfW - 0.5_cm; }
  float CardTopM() const { return kBaseY + kHistH; }  // the tallest bar = the top of the outline
  Rect PreviewRectM() const {
    float top = kBaseY - 1.25_cm;  // clearance for the ramp + value chip under the baseline
    return Rect(-kHalfW + 0.34_cm, top - 2.7_cm, kHalfW - 0.34_cm, top);
  }
  float RunCenterY() const { return PreviewRectM().bottom - 0.2_cm - ui::slop::RunButton::kRadius; }
  float CardBottomM() const { return RunCenterY() - ui::slop::RunButton::kRadius - 0.3_cm; }

  float MarkerXM() const {
    return HistXL() + std::clamp((level - 1.f) / 253.f, 0.f, 1.f) * (HistXR() - HistXL());
  }
  float XToLevel(float mx) const {
    float t = std::clamp((mx - HistXL()) / std::max(1e-4f, HistXR() - HistXL()), 0.f, 1.f);
    return 1.f + t * 253.f;
  }

  // The outline's top edge is the live histogram. The control column and the
  // blade are unioned in: the pointer only reaches what Shape() contains.
  SkPath Shape() const override {
    Rect body(-kHalfW, CardBottomM(), kHalfW, kBaseY + 0.12_cm);
    SkPathBuilder b;
    b.addRRect(RRect::MakeSimple(body, 3_mm).sk);
    float xl = HistXL(), xr = HistXR(), bw = (xr - xl) / kSkyBins;
    for (int i = 0; i < kSkyBins; ++i) {
      float hh = 0.12_cm + skyline[i] * (kHistH - 0.12_cm);
      float bl = xl + i * bw;
      b.addRRect(
          RRect::MakeSimple(Rect(bl + 0.3_mm, kBaseY, bl + bw - 0.3_mm, kBaseY + hh), 0.5_mm).sk);
    }
    b.addRRect(RRect::MakeSimple(Rect(kHalfW - 0.1_cm, MethodCellM(3).bottom - 0.08_cm,
                                      kHalfW + 1.16_cm, kBaseY + 0.12_cm),
                                 1.5_mm)
                   .sk);
    float mx = MarkerXM();
    b.addRRect(
        RRect::MakeSimple(Rect(mx - 0.2_cm, kBaseY, mx + 0.2_cm, CardTopM() + 0.2_cm), 2_mm).sk);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::RRect(RRect::MakeSimple(body, 3_mm).sk);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl)) {
      Rect p = PreviewRectM();
      return {.pos = {-kHalfW, p.CenterY()}, .dir = 180_deg};
    }
    if (&arg == static_cast<const Interface::Table*>(&Threshold::level_src_tbl)) {
      Rect p = PreviewRectM();
      return {.pos = {-kHalfW, p.CenterY() - 1.1_cm}, .dir = 180_deg};
    }
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  Rect MethodCellM(int which) const {
    float top = kBaseY - 0.6_cm - which * 0.52_cm;
    return Rect(kHalfW + 0.08_cm, top - 0.44_cm, kHalfW + 1.12_cm, top);
  }

  uint32_t ComputeParamHash() const {
    uint32_t hash = 2166136261u;
    if (auto tool = LockTool()) {
      auto lock = std::lock_guard(tool->mutex);
      for (int i = 0; i < PhotoTool::kMaxParams; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &tool->params[i], sizeof(float));
        hash ^= bits;
        hash *= 16777619u;
      }
    }
    return hash;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    histogram.assign(256, 0);
    max_log_count = 1.f;
    for (auto& s : skyline) s = 0.f;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider image_prov = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = image_prov ? image_prov.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scale = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scale)), dh = std::max(1, (int)(sh * scale));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    {
      int w = pixGetWidth(pix), h = pixGetHeight(pix);
      l_int32 wpl = pixGetWpl(pix);
      l_uint32* d = pixGetData(pix);
      for (int y = 0; y < h; ++y) {
        l_uint32* line = d + (size_t)y * wpl;
        for (int x = 0; x < w; ++x) {
          uint32_t rgba = __builtin_bswap32(line[x]);
          float r = (rgba >> 24) & 0xff, gg = (rgba >> 16) & 0xff, b = (rgba >> 8) & 0xff;
          int lum = (int)(0.3f * r + 0.59f * gg + 0.11f * b);
          histogram[std::clamp(lum, 0, 255)]++;
        }
      }
      for (uint32_t c : histogram)
        if (c > 0) max_log_count = std::max(max_log_count, std::log((float)c + 1.f));
      for (int i = 0; i < kSkyBins; ++i) {
        int lo = i * 256 / kSkyBins, hi = (i + 1) * 256 / kSkyBins;
        double acc = 0;
        int n = 0;
        for (int s = lo; s < hi && s < 256; ++s) {
          acc += std::log((float)histogram[s] + 1.f);
          ++n;
        }
        skyline[i] =
            (max_log_count > 1.f && n > 0) ? (float)(acc / (double)n) / max_log_count : 0.f;
      }
    }
    float snap[PhotoTool::kMaxParams];
    {
      auto lock = std::lock_guard(tool->mutex);
      for (int i = 0; i < PhotoTool::kMaxParams; ++i) snap[i] = tool->params[i];
    }
    // OTSU: measure the found global threshold on the proxy for the blade readout.
    found_level = -1;
    if (method == 1) {
      Pix* g8 = pixConvertRGBToGray(pix, 0.3f, 0.59f, 0.11f);
      if (g8) {
        Pix* pixth = nullptr;
        pixOtsuAdaptiveThreshold(g8, pixGetWidth(g8), pixGetHeight(g8), 0, 0, 0.f, &pixth, nullptr);
        if (pixth) {
          l_uint32 v = 128;
          pixGetPixel(pixth, 0, 0, &v);
          found_level = (int)v;
          pixDestroy(&pixth);
        }
        pixDestroy(&g8);
      }
    }
    Pix* result = tool->ApplyOp(pix, snap);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto tool = LockTool()) {
      std::string l(tool->Label());
      if (l != label) label = l;
      uint32_t hash = ComputeParamHash();
      if (hash != preview_param_hash) {
        preview_param_hash = hash;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*tool) != preview_source_id) preview_dirty = true;
      float dv = 0.f;
      bool was_driven = driven;
      driven = false;
      if (auto th = LockObject<Threshold>()) driven = th->DrivenLevel(dv);
      if (driven != was_driven) preview_dirty = true;
      if (driven) {
        uint32_t dbits = (uint32_t)std::lround(dv * 16.f) ^ 0x9E3779B9u;
        if (dbits != driven_hash) {
          driven_hash = dbits;
          preview_dirty = true;
        }
        level = dv;  // the blade parks at the incoming value
      }
      {
        bool bfg = false;
        int meth = 0;
        if (auto th = LockObject<Threshold>()) {
          auto lock = std::lock_guard(th->mutex);
          bfg = th->bright_fg;
          meth = th->method;
        }
        if (bfg != bright_fg) {
          bright_fg = bfg;
          preview_dirty = true;
        }
        if (meth != method) {
          method = meth;
          preview_dirty = true;
        }
      }
      if (preview_dirty) RecomputePreview();
      if (method == 1 && found_level >= 0) {
        level = (float)found_level;  // OTSU: the blade parks at the found threshold
      } else if (!driven) {
        auto lock = std::lock_guard(tool->mutex);
        level = tool->params[0];
      }
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    float mx = MarkerXM();

    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    {
      canvas.save();
      canvas.clipPath(shape, true);
      float top = CardTopM() + 0.2_cm;
      SkPaint li;
      li.setColor(bright_fg ? 0x1affffff : 0x24000000);
      SkPaint ri;
      ri.setColor(bright_fg ? 0x24000000 : 0x1affffff);
      canvas.drawRect(Rect(HistXL() - 0.2_cm, kBaseY - 0.06_cm, mx, top).sk, li);
      canvas.drawRect(Rect(mx, kBaseY - 0.06_cm, HistXR() + 0.2_cm, top).sk, ri);
      canvas.restore();
    }

    Rect pr = PreviewRectM();
    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, pr);
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#d8cdbb"_color);
      canvas.drawRect(pr.sk, ph);
      SkPaint key;
      key.setAntiAlias(true);
      key.setStyle(SkPaint::kStroke_Style);
      key.setStrokeWidth(0.3_mm);
      key.setColor("#3a342c"_color);
      canvas.drawRect(pr.sk, key);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto PX = [&](float m) { return m / kPxToMetric; };

      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0x7A1, 1);
      DrawDepthChipPx(canvas,
                      SkRect::MakeLTRB(pr.left / kPxToMetric, -pr.top / kPxToMetric,
                                       pr.right / kPxToMetric, -pr.bottom / kPxToMetric),
                      cached_preview, out_depth, out_cmap, 0x7A9);

      int steps = 12;
      float rTop = kBaseY - 0.06_cm, rBot = kBaseY - 0.24_cm;
      for (int i = 0; i < steps; ++i) {
        float x0 = HistXL() + (HistXR() - HistXL()) * i / steps;
        float x1 = HistXL() + (HistXR() - HistXL()) * (i + 1) / steps;
        uint8_t gv = (uint8_t)std::lround(255.f * i / (steps - 1));
        SkPaint sp;
        sp.setColor(SkColorSetARGB(255, gv, gv, gv));
        canvas.drawRect(SkRect::MakeLTRB(PX(x0), -rTop / kPxToMetric, PX(x1), -rBot / kPxToMetric),
                        sp);
      }
      canvas.drawRect(
          SkRect::MakeLTRB(PX(HistXL()), -rTop / kPxToMetric, PX(HistXR()), -rBot / kPxToMetric),
          slop::InkPaint(slop::kInk, slop::kStrokeHair));

      float topY = CardTopM() + 0.05_cm, knobY = kBaseY - 0.16_cm;
      SkPath blade =
          slop::WobbleLine(P(mx, topY), P(mx, knobY), slop::kWonk * 0.7f, slop::kSeg, 0x7B1);
      slop::HandShadow(canvas, blade, {slop::kShadowDX * 0.5f, slop::kShadowDY * 0.5f},
                       slop::kShadow, 0x7B2);
      slop::SketchyStroke(canvas, blade, slop::kInk, slop::kStrokeBold, 0x7B3, 2);
      float knobR = PX(0.32_cm);
      SkPath knob = slop::WobbleEllipse(P(mx, knobY), knobR, knobR * 0.94f, slop::kWonk, 0x7B4, 36);
      bool blade_readout = driven || method == 1;
      bool blade_na = method == 2;
      slop::MisregFill(
          canvas, knob,
          (blade_readout || blade_na) ? slop::kGray : (dragging ? slop::kRed : slop::kYellow),
          0x7B5);
      slop::SketchyStroke(canvas, knob, slop::kInk, slop::kStrokeBold, 0x7B6, 2);

      char buf[16];
      if (method == 2)
        snprintf(buf, sizeof(buf), "-");
      else
        snprintf(buf, sizeof(buf), "%d", (int)std::lround(level));
      float fs = 26.f, tw = slop::TextWidth(buf, fs), cw = tw + PX(0.34_cm), ch = fs * 1.5f;
      float cxm = std::clamp(mx, -kHalfW + 0.75_cm, kHalfW - 0.75_cm);
      SkPoint cc = P(cxm, kBaseY - 0.74_cm);
      SkRect chip = SkRect::MakeXYWH(cc.fX - cw / 2, cc.fY - ch / 2, cw, ch);
      SkPath cp = slop::WonkyRoundRect(chip, ch * 0.35f, slop::kWonk, 0x7C1);
      slop::HandShadow(canvas, cp, {slop::kShadowDX * 0.5f, slop::kShadowDY * 0.5f}, slop::kShadow,
                       0x7C2);
      slop::MisregFill(canvas, cp, (blade_readout || blade_na) ? slop::kGray : slop::kYellow,
                       0x7C3);
      slop::SketchyStroke(canvas, cp, slop::kInk, slop::kStroke, 0x7C4, 1);
      slop::DrawText(canvas, buf, {cc.fX - tw / 2, cc.fY + fs * 0.36f}, fs, slop::kInk, false, 0);
      if (driven || method == 1 || method == 2) {
        slop::HatchRect(canvas, chip, slop::kInkSoft, 7.f, 0x7C5);
        const char* dl = method == 2 ? "LOCAL" : (method == 1 ? "FOUND" : "DRIVEN");
        float dw = slop::TextWidth(dl, 11.f);
        slop::DrawText(canvas, dl, {cc.fX - dw / 2, cc.fY + ch / 2 + 12.f}, 11.f, kLabelInk, false,
                       0);
      }

      {
        SkRect pill = SkRect::MakeLTRB(PX(kHalfW + 0.12_cm), -(kBaseY - 0.02_cm) / kPxToMetric,
                                       PX(kHalfW + 1.0_cm), -(kBaseY - 0.34_cm) / kPxToMetric);
        ui::leptonica::DrawPolarity(canvas, pill, bright_fg, slop::State::Default, 0x7E0);
        const char* pl = "INK";
        float pw2 = slop::TextWidth(pl, 11.f);
        slop::DrawText(canvas, pl, {pill.centerX() - pw2 / 2, pill.fTop - 6.f}, 11.f, kLabelInk,
                       false, 0);
      }

      {
        static const char* const kMeth[4] = {"FIXED", "OTSU", "SAUVOLA", "COMPS"};
        for (int which = 0; which < 4; ++which) {
          SkRect cell = SkRect::MakeLTRB(
              PX(MethodCellM(which).left), -MethodCellM(which).top / kPxToMetric,
              PX(MethodCellM(which).right), -MethodCellM(which).bottom / kPxToMetric);
          bool sel = method == which;
          SkPath cp = slop::WonkyRoundRect(cell, cell.height() * 0.3f, slop::kWonk * 0.5f,
                                           0x7F0u + (uint32_t)which);
          if (sel)
            slop::HandShadow(canvas, cp, {2.f, 2.f}, slop::kShadow, 0x7F4u + (uint32_t)which);
          slop::MisregFill(canvas, cp, sel ? slop::kPaper : slop::kGray, 0x7F8u + (uint32_t)which);
          slop::SketchyStroke(canvas, cp, slop::kInk, sel ? slop::kStroke : slop::kStrokeHair,
                              0x7FCu + (uint32_t)which, 1);
          float fs2 = 11.f;
          float lw = slop::TextWidth(kMeth[which], fs2);
          slop::DrawText(canvas, kMeth[which], {cell.centerX() - lw / 2, cell.centerY() + 4.f}, fs2,
                         sel ? slop::kInk : slop::kInkSoft, false, 0);
          if (sel) slop::Highlight(canvas, cell, slop::kBlue, 0x7F2);
        }
      }

      std::string title = label.empty() ? std::string("THRESHOLD") : label;
      float titleCx = -0.9_cm;
      float tpx = slop::TextWidth(title, kTitleTextPx);
      slop::DrawText(canvas, title, P(titleCx - tpx * kPxToMetric * 0.5f, RunCenterY() + 0.26_cm),
                     kTitleTextPx, slop::kInk, true, 0x7D1);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fpx = slop::TextWidth(credit, kCreditTextPx);
        slop::DrawText(canvas, credit,
                       P(titleCx - fpx * kPxToMetric * 0.5f, RunCenterY() - 0.34_cm), kCreditTextPx,
                       "#8a7d66"_color, false, 0x7D2);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      float mx = MarkerXM();
      bool near_blade = std::abs(pos.x - mx) <= 0.45_cm && pos.y >= kBaseY - 1.0_cm &&
                        pos.y <= CardTopM() + 0.2_cm;
      bool on_knob = std::hypot(pos.x - mx, pos.y - (kBaseY - 0.16_cm)) <= 0.5_cm;
      if ((near_blade || on_knob) && !driven && method == 0)
        return std::make_unique<ThresholdBladeDrag>(p, *this);
      for (int which = 0; which < 4; ++which) {
        Rect r = MethodCellM(which);
        if (pos.x >= r.left && pos.x <= r.right && pos.y >= r.bottom && pos.y <= r.top) {
          if (auto th = LockObject<Threshold>()) {
            {
              auto lock = std::lock_guard(th->mutex);
              th->method = which;
            }
            th->WakeToys();
          }
          return std::make_unique<ThresholdPolarityPoke>(p);
        }
      }
      if (pos.x >= kHalfW + 0.02_cm && pos.x <= kHalfW + 1.1_cm && pos.y >= kBaseY - 0.44_cm &&
          pos.y <= kBaseY + 0.08_cm) {
        if (auto th = LockObject<Threshold>()) {
          {
            auto lock = std::lock_guard(th->mutex);
            th->bright_fg = !th->bright_fg;
          }
          th->WakeToys();
        }
        return std::make_unique<ThresholdPolarityPoke>(p);
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

ThresholdBladeDrag::ThresholdBladeDrag(ui::Pointer& p, ThresholdToy& w) : Action(p), widget(&w) {
  if (widget) widget->dragging = true;
}
ThresholdBladeDrag::~ThresholdBladeDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void ThresholdBladeDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  float v = std::clamp(widget->XToLevel(pos.x), 1.f, 254.f);
  widget->level = v;
  if (auto tool = widget->LockTool()) {
    {
      auto lock = std::lock_guard(tool->mutex);
      tool->params[0] = v;
    }
    tool->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Threshold::MakeToy(ui::Widget* parent) {
  return std::make_unique<ThresholdToy>(parent, *this);
}

// ============================================================================
// Morphology
// ============================================================================
Pix* Morphology::ApplyOp(Pix* in, const float*) const {
  int w, h, mode, ht, conn;
  bool pk, clr;
  uint8_t c[kMaxN * kMaxN];
  {
    auto lock = std::lock_guard(mutex);
    w = std::clamp(sel_w, 1, kMaxN);
    h = std::clamp(sel_h, 1, kMaxN);
    mode = op_mode;
    ht = std::clamp(height, 1, 255);
    pk = peaks;
    conn = connectivity == 4 ? 4 : 8;
    clr = color;
    for (int i = 0; i < kMaxN * kMaxN; ++i) c[i] = cells[i];
  }
  if (clr && mode <= 3) {
    // COLOR morphology: dilate/erode/open/close each channel; brick-only (the grid sets size).
    static const int kTypes[] = {L_MORPH_DILATE, L_MORPH_ERODE, L_MORPH_OPEN, L_MORPH_CLOSE};
    return pixColorMorph(in, kTypes[mode], w | 1, h | 1);
  }
  Pix* gray = pixConvertRGBToGray(in, 0.3f, 0.59f, 0.11f);
  if (!gray) return nullptr;
  if (mode == 4 || mode == 5) {
    // gray morphology: lift peaks (or valleys) off the local background. 8 bpp out.
    Pix* res = nullptr;
    if (mode == 4) {
      res = pixTophat(gray, w | 1, h | 1, pk ? L_TOPHAT_WHITE : L_TOPHAT_BLACK);
    } else {
      Pix* relief = pk ? gray : pixInvert(nullptr, gray);  // basins = domes of inverted relief
      if (relief) res = pixHDome(relief, ht, 8);
      if (relief != gray) pixDestroy(&relief);
    }
    pixDestroy(&gray);
    return res;
  }
  Pix* bin = pixThresholdToBinary(gray, 128);
  pixDestroy(&gray);
  if (!bin) return nullptr;
  if (mode == 7) {
    // THIN: skeletonize. In ink space the bright subject is the BACKGROUND, so the polarity
    // chip's bright end maps to L_THIN_BG (thin the white shapes down to their skeletons).
    Pix* res = pixThinConnected(bin, pk ? L_THIN_BG : L_THIN_FG, conn, 0);
    pixDestroy(&bin);
    return res;
  }
  SEL* sel = selCreate(h, w, nullptr);
  if (!sel) {
    pixDestroy(&bin);
    return nullptr;
  }
  bool any_hit = false;
  for (int j = 0; j < h; ++j)
    for (int i = 0; i < w; ++i) {
      int v = c[j * w + i];
      if (v == 1) {
        selSetElement(sel, j, i, SEL_HIT);
        any_hit = true;
      } else if (v == 2 && mode == 6) {
        selSetElement(sel, j, i, SEL_MISS);  // binary ops ignore MISS cells; HMT needs them
      }
    }
  if (!any_hit) selSetElement(sel, h / 2, w / 2, SEL_HIT);  // a Sel without hits is invalid
  selSetOrigin(sel, h / 2, w / 2);
  Pix* res = nullptr;
  switch (mode) {
    case 1:
      res = pixErode(nullptr, bin, sel);
      break;
    case 2:
      res = pixOpen(nullptr, bin, sel);
      break;
    case 3:
      res = pixClose(nullptr, bin, sel);
      break;
    case 6:
      res = pixHMT(nullptr, bin, sel);
      break;
    default:
      res = pixDilate(nullptr, bin, sel);
      break;
  }
  selDestroy(&sel);
  pixDestroy(&bin);
  return res;  // native 1 bpp - preview + surface upconvert
}

void Morphology::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("op_mode");
  writer.Int(op_mode);
  writer.Key("sel_w");
  writer.Int(sel_w);
  writer.Key("sel_h");
  writer.Int(sel_h);
  writer.Key("height");
  writer.Int(height);
  writer.Key("peaks");
  writer.Bool(peaks);
  writer.Key("connectivity");
  writer.Int(connectivity);
  writer.Key("color");
  writer.Bool(color);
  char buf[kMaxN * kMaxN + 1];
  int n = std::clamp(sel_w, 1, kMaxN) * std::clamp(sel_h, 1, kMaxN);
  for (int i = 0; i < n; ++i) buf[i] = (char)('0' + std::clamp((int)cells[i], 0, 2));
  buf[n] = 0;
  writer.Key("cells");
  writer.String(buf, n);
}
bool Morphology::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "op_mode") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) op_mode = std::clamp(v, 0, 7);
  } else if (key == "sel_w") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) sel_w = std::clamp(v, 1, kMaxN);
  } else if (key == "sel_h") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) sel_h = std::clamp(v, 1, kMaxN);
  } else if (key == "height") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) height = std::clamp(v, 1, 255);
  } else if (key == "peaks") {
    bool v = true;
    d.Get(v, status);
    if (OK(status)) peaks = v;
  } else if (key == "connectivity") {
    int v = 8;
    d.Get(v, status);
    if (OK(status)) connectivity = v == 4 ? 4 : 8;
  } else if (key == "color") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) color = v;
  } else if (key == "cells") {
    Str s;
    d.Get(s, status);
    if (OK(status)) {
      int n = std::min((int)s.size(), kMaxN * kMaxN);
      for (int i = 0; i < n; ++i) cells[i] = (uint8_t)std::clamp(s[i] - '0', 0, 2);
    }
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct MorphologyToy;

// A click that toggles a Stamp cell or selects a Mode-wheel option (mutation done in FindAction).
struct MorphPoke : Action {
  MorphPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

// Dragging the per-mode parameter row: BRICK size (TOPHAT) or HEIGHT (DOME).
struct MorphLevelDrag : Action {
  TrackedPtr<MorphologyToy> widget;
  MorphLevelDrag(ui::Pointer& p, MorphologyToy& w);
  void Update() override;
};

// Before/after glyphs drawn on the wheel face for the selected mode.
static const SkPath* MorphWheelGlyphs() {
  static const std::array<SkPath, 8> paths = [] {
    std::array<SkPath, 8> g;
    auto arrow = [](SkPathBuilder& b) {  // small right-pointing triangle at the box centre
      b.moveTo(-0.16f, -0.18f);
      b.lineTo(0.2f, 0);
      b.lineTo(-0.16f, 0.18f);
      b.close();
    };
    auto sq = [](SkPathBuilder& b, float cx, float half) {
      b.addRect(SkRect::MakeLTRB(cx - half, -half, cx + half, half));
    };
    {  // DILATE: small -> big
      SkPathBuilder b;
      sq(b, -0.85f, 0.22f);
      arrow(b);
      sq(b, 0.85f, 0.5f);
      g[0] = b.detach();
    }
    {  // ERODE: big -> small
      SkPathBuilder b;
      sq(b, -0.85f, 0.5f);
      arrow(b);
      sq(b, 0.85f, 0.22f);
      g[1] = b.detach();
    }
    {  // OPEN: blob + speck -> blob alone
      SkPathBuilder b;
      sq(b, -0.95f, 0.4f);
      b.addCircle(-0.3f, -0.38f, 0.14f);
      arrow(b);
      sq(b, 0.85f, 0.4f);
      g[2] = b.detach();
    }
    {  // CLOSE: holed blob -> solid blob (even-odd punches the hole)
      SkPathBuilder b;
      b.setFillType(SkPathFillType::kEvenOdd);
      sq(b, -0.85f, 0.5f);
      b.addRect(SkRect::MakeLTRB(-1.02f, -0.16f, -0.68f, 0.16f));
      arrow(b);
      sq(b, 0.85f, 0.5f);
      g[3] = b.detach();
    }
    {  // TOPHAT: the hat itself
      SkPathBuilder b;
      b.addRect(SkRect::MakeLTRB(-0.9f, 0.3f, 0.9f, 0.52f));
      b.addRect(SkRect::MakeLTRB(-0.45f, -0.55f, 0.45f, 0.36f));
      g[4] = b.detach();
    }
    {  // DOME: a dome on its base
      SkPathBuilder b;
      b.moveTo(-0.8f, 0.34f);
      b.quadTo(0, -0.9f, 0.8f, 0.34f);
      b.close();
      b.addRect(SkRect::MakeLTRB(-1.05f, 0.34f, 1.05f, 0.52f));
      g[5] = b.detach();
    }
    {  // HMT: the matched 3x3 pattern
      SkPathBuilder b;
      const float s = 0.36f;
      const int fill[3][3] = {{1, 0, 1}, {0, 1, 0}, {1, 0, 0}};
      for (int r = 0; r < 3; ++r)
        for (int col = 0; col < 3; ++col)
          if (fill[r][col])
            b.addRect(SkRect::MakeXYWH(-0.54f + col * s, -0.54f + r * s, s * 0.8f, s * 0.8f));
      g[6] = b.detach();
    }
    {  // THIN: thick stroke -> thin stroke
      SkPathBuilder b;
      b.addRect(SkRect::MakeLTRB(-1.1f, -0.5f, -0.6f, 0.5f));
      arrow(b);
      b.addRect(SkRect::MakeLTRB(0.78f, -0.5f, 0.92f, 0.5f));
      g[7] = b.detach();
    }
    return g;
  }();
  return paths.data();
}

struct MorphologyToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  // UI-thread mirror of the Sel grid + mode (synced in Tick).
  int sel_w = 3, sel_h = 3, op_mode = 0;
  int height = 64;
  bool peaks = true;
  int connectivity = 8;
  bool color = false;
  uint8_t cells[Morphology::kMaxN * Morphology::kMaxN] = {};

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.3_cm;
  constexpr static float kFaceTop = 2.25_cm;
  constexpr static float kHandleR = 0.6_cm;

  Ptr<Morphology> LockMorph() const { return LockObject<Morphology>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  MorphologyToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto tool = LockObject<PhotoTool>()) tool->develop->ScheduleRun();
    });
    if (auto m = LockMorph()) {
      fn_credit = std::string(m->LeptonicaFn());
      auto lock = std::lock_guard(m->mutex);
      sel_w = m->sel_w;
      sel_h = m->sel_h;
      op_mode = m->op_mode;
      for (int i = 0; i < Morphology::kMaxN * Morphology::kMaxN; ++i) cells[i] = m->cells[i];
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect GridRectM() const { return Rect(-kHalfW + 0.4_cm, 0.05_cm, -kHalfW + 2.3_cm, 1.95_cm); }
  float WheelCX() const { return kHalfW - 1.7_cm; }
  float WheelCY() const { return 0.85_cm; }
  float WheelR() const { return 0.78_cm; }
  Rect PolarityRectM() const { return Rect(-1.7_cm, -0.46_cm, -0.2_cm, -0.06_cm); }
  Rect PreviewRectM() const { return Rect(-kHalfW + 0.35_cm, -2.7_cm, kHalfW - 0.35_cm, -0.55_cm); }
  Rect ParamRowM() const { return Rect(-kHalfW + 0.4_cm, -3.35_cm, 1.2_cm, -2.95_cm); }
  Rect ConnRectM() const { return Rect(-kHalfW + 0.4_cm, -3.32_cm, -kHalfW + 2.4_cm, -2.92_cm); }
  float RunCenterY() const { return -3.5_cm; }
  bool GrayMode() const { return op_mode == 4 || op_mode == 5; }
  bool PolarityApplies() const { return op_mode == 4 || op_mode == 5 || op_mode == 7; }
  bool SelApplies() const { return (op_mode <= 3 && !color) || op_mode == 6; }
  bool ColorApplies() const { return op_mode <= 3; }
  float TitleY() const { return -3.88_cm; }
  float FaceBottom() const { return -5.3_cm; }
  float HandleCY() const { return kFaceTop + 0.55_cm + kHandleR; }

  SkRect GridRectPx() const {
    Rect g = GridRectM();
    return SkRect::MakeLTRB(g.left / kPxToMetric, -g.top / kPxToMetric, g.right / kPxToMetric,
                            -g.bottom / kPxToMetric);
  }
  SkPoint WheelCenterPx() const { return {WheelCX() / kPxToMetric, -WheelCY() / kPxToMetric}; }
  float WheelRPx() const { return WheelR() / kPxToMetric; }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRRect(RRect::MakeSimple(Rect(-kHalfW, FaceBottom(), kHalfW, kFaceTop), 4_mm).sk);
    float ny0 = kFaceTop - 0.05_cm, ny1 = HandleCY() - kHandleR * 0.55f;
    SkPath neck = SkPathBuilder()
                      .moveTo(-1.0_cm, ny0)
                      .lineTo(1.0_cm, ny0)
                      .lineTo(0.7_cm, ny1)
                      .lineTo(-0.7_cm, ny1)
                      .close()
                      .detach();
    b.addPath(neck);
    b.addOval(SkRect::MakeXYWH(-kHandleR, HandleCY() - kHandleR, 2 * kHandleR, 2 * kHandleR));
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::RRect(RRect::MakeSimple(Rect(-kHalfW, FaceBottom(), kHalfW, kFaceTop), 4_mm).sk);
  }

  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, FaceBottom() - 0.6_cm, kHalfW + 0.5_cm,
                HandleCY() + kHandleR + 0.4_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl)) {
      Rect p = PreviewRectM();
      return {.pos = {p.left, p.CenterY()}, .dir = 180_deg};
    }
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t MorphHash() const {
    uint32_t h = 2166136261u;
    if (auto m = LockMorph()) {
      auto lock = std::lock_guard(m->mutex);
      auto mix = [&](uint32_t v) {
        h ^= v;
        h *= 16777619u;
      };
      mix((uint32_t)m->sel_w);
      mix((uint32_t)m->sel_h);
      mix((uint32_t)m->op_mode);
      mix((uint32_t)m->height);
      mix((uint32_t)m->peaks);
      mix((uint32_t)m->connectivity);
      mix((uint32_t)m->color);
      for (int i = 0; i < Morphology::kMaxN * Morphology::kMaxN; ++i) mix(m->cells[i]);
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scale = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scale)), dh = std::max(1, (int)(sh * scale));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto m = LockMorph()) {
      uint32_t h = MorphHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*m) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(m->mutex);
        sel_w = std::clamp(m->sel_w, 1, Morphology::kMaxN);
        sel_h = std::clamp(m->sel_h, 1, Morphology::kMaxN);
        op_mode = m->op_mode;
        height = m->height;
        peaks = m->peaks;
        connectivity = m->connectivity;
        color = m->color;
        for (int i = 0; i < Morphology::kMaxN * Morphology::kMaxN; ++i) cells[i] = m->cells[i];
      }
      fn_credit = std::string(m->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    Rect pr = PreviewRectM();
    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, pr);
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#d8cdbb"_color);
      canvas.drawRect(pr.sk, ph);
      SkPaint key;
      key.setAntiAlias(true);
      key.setStyle(SkPaint::kStroke_Style);
      key.setStrokeWidth(0.3_mm);
      key.setColor("#3a342c"_color);
      canvas.drawRect(pr.sk, key);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0x9A1, 1);
      DrawDepthChipPx(canvas,
                      SkRect::MakeLTRB(pr.left / kPxToMetric, -pr.top / kPxToMetric,
                                       pr.right / kPxToMetric, -pr.bottom / kPxToMetric),
                      cached_preview, out_depth, out_cmap, 0x9A9);
      float hy = HandleCY();
      for (int i = -1; i <= 1; ++i)
        canvas.drawPath(
            slop::WobbleLine(P(-0.34_cm, hy + i * 0.18_cm), P(0.34_cm, hy + i * 0.18_cm),
                             slop::kWonk * 0.5f, slop::kSeg, slop::Hash2(0x9B0, i + 4)),
            slop::InkPaint(slop::kInk, slop::kStrokeHair));
      if (op_mode == 4 || (color && op_mode <= 3)) {
        uint8_t solid[Morphology::kMaxN * Morphology::kMaxN];
        for (int i = 0; i < sel_w * sel_h; ++i) solid[i] = 1;
        ui::leptonica::DrawStamp(canvas, GridRectPx(), solid, sel_w, sel_h, sel_w / 2, sel_h / 2,
                                 slop::State::Disabled, 0x9C0);
      } else {
        ui::leptonica::DrawStamp(canvas, GridRectPx(), cells, sel_w, sel_h, sel_w / 2, sel_h / 2,
                                 SelApplies() ? slop::State::Default : slop::State::Disabled,
                                 0x9C0);
      }
      Rect gm = GridRectM();
      {
        const char* gl = (op_mode == 4 || (color && op_mode <= 3)) ? "BRICK"
                         : op_mode == 6                            ? "HIT\xc2\xb7MISS"
                                                                   : "PATTERN";
        float fs = 20.f, tw = slop::TextWidth(gl, fs);
        slop::DrawText(canvas, gl, P(gm.CenterX() - tw * kPxToMetric * 0.5f, gm.bottom - 0.34_cm),
                       fs, kLabelInk, false, 0);
      }
      const char* morph[] = {"DILATE", "ERODE", "OPEN", "CLOSE", "TOPHAT", "DOME", "HMT", "THIN"};
      ui::leptonica::DrawModeWheel(canvas, WheelCenterPx(), WheelRPx(), morph, 8, op_mode,
                                   slop::State::Default, 0x9D0, MorphWheelGlyphs());
      if (ColorApplies()) {
        Rect pm = PolarityRectM();
        SkRect tpx = SkRect::MakeLTRB(pm.left / kPxToMetric, -pm.top / kPxToMetric,
                                      (pm.left + 0.8_cm) / kPxToMetric, -pm.bottom / kPxToMetric);
        slop::Toggle(canvas, tpx, color, slop::State::Default, 0x9D9);
        slop::DrawText(canvas, "COLOR", {tpx.right() + 6.f, tpx.centerY() + 5.f}, 13.f, kLabelInk,
                       false, 0);
      }
      if (PolarityApplies()) {
        Rect pm = PolarityRectM();
        ui::leptonica::DrawPolarity(
            canvas,
            SkRect::MakeLTRB(pm.left / kPxToMetric, -pm.top / kPxToMetric, pm.right / kPxToMetric,
                             -pm.bottom / kPxToMetric),
            peaks, slop::State::Default, 0x9D5);
      }
      if (op_mode != 7) {
        Rect rm = ParamRowM();
        SkRect rpx = SkRect::MakeLTRB(rm.left / kPxToMetric, -rm.top / kPxToMetric,
                                      rm.right / kPxToMetric, -rm.bottom / kPxToMetric);
        char buf[24];
        float frac;
        if (op_mode == 5) {
          frac = (height - 1) / 254.f;
          snprintf(buf, sizeof(buf), "HEIGHT %d", height);
        } else if (op_mode == 4 || (color && op_mode <= 3)) {
          frac = (std::max(sel_w, 1) - 1) / float(Morphology::kMaxN - 1);
          snprintf(buf, sizeof(buf), "BRICK %dx%d", sel_w | 1, sel_h | 1);
        } else {
          frac = (std::max(sel_w, 1) - 1) / float(Morphology::kMaxN - 1);
          snprintf(buf, sizeof(buf), "SIZE %dx%d", sel_w, sel_h);
        }
        slop::Slider(canvas, rpx, std::clamp(frac, 0.f, 1.f), slop::State::Default, 0x9D7);
        slop::DrawText(canvas, buf, P(rm.left, rm.top + 0.12_cm), 15.f, kLabelInk, false, 0);
      } else {
        Rect rm = ConnRectM();
        ui::leptonica::DrawConnectivity(
            canvas,
            SkRect::MakeLTRB(rm.left / kPxToMetric, -rm.top / kPxToMetric, rm.right / kPxToMetric,
                             -rm.bottom / kPxToMetric),
            connectivity == 8, slop::State::Default, 0x9D8);
      }
      std::string title = "MORPHOLOGY";
      float tcx = -1.0_cm;
      float tpx = slop::TextWidth(title, kTitleTextPx);
      slop::DrawText(canvas, title, P(tcx - tpx * kPxToMetric * 0.5f, TitleY()), kTitleTextPx,
                     slop::kInk, true, 0x9E0);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fpx = slop::TextWidth(credit, kCreditTextPx);
        slop::DrawText(canvas, credit, P(tcx - fpx * kPxToMetric * 0.5f, TitleY() - 0.52_cm),
                       kCreditTextPx, "#8a7d66"_color, false, 0x9E1);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint pp = {pos.x / kPxToMetric, -pos.y / kPxToMetric};
      int cx, cy;
      if (SelApplies() && ui::leptonica::StampCellAt(GridRectPx(), pp, sel_w, sel_h, cx, cy)) {
        if (auto m = LockMorph()) {
          {
            auto lock = std::lock_guard(m->mutex);
            int idx = cy * m->sel_w + cx;
            if (idx >= 0 && idx < Morphology::kMaxN * Morphology::kMaxN) {
              if (m->op_mode == 6)
                m->cells[idx] = (m->cells[idx] + 1) % 3;
              else
                m->cells[idx] = m->cells[idx] ? 0 : 1;
            }
          }
          m->WakeToys();
        }
        return std::make_unique<MorphPoke>(p);
      }
      int mode = ui::leptonica::ModeWheelHit(WheelCenterPx(), WheelRPx(), pp, 8);
      if (mode >= 0) {
        if (auto m = LockMorph()) {
          {
            auto lock = std::lock_guard(m->mutex);
            m->op_mode = mode;
          }
          m->WakeToys();
        }
        return std::make_unique<MorphPoke>(p);
      }
      if (ColorApplies()) {
        Rect pm = PolarityRectM();
        if (pos.x >= pm.left && pos.x <= pm.left + 0.9_cm && pos.y >= pm.bottom - 0.1_cm &&
            pos.y <= pm.top + 0.1_cm) {
          if (auto m = LockMorph()) {
            {
              auto lock = std::lock_guard(m->mutex);
              m->color = !m->color;
            }
            m->WakeToys();
          }
          return std::make_unique<MorphPoke>(p);
        }
      }
      if (PolarityApplies()) {
        Rect pm = PolarityRectM();
        if (pos.x >= pm.left && pos.x <= pm.right && pos.y >= pm.bottom - 0.1_cm &&
            pos.y <= pm.top + 0.1_cm) {
          if (auto m = LockMorph()) {
            {
              auto lock = std::lock_guard(m->mutex);
              m->peaks = !m->peaks;
            }
            m->WakeToys();
          }
          return std::make_unique<MorphPoke>(p);
        }
      }
      if (op_mode != 7) {
        Rect rm = ParamRowM();
        if (pos.x >= rm.left - 0.2_cm && pos.x <= rm.right + 0.2_cm &&
            pos.y >= rm.bottom - 0.2_cm && pos.y <= rm.top + 0.2_cm)
          return std::make_unique<MorphLevelDrag>(p, *this);
      } else {
        Rect rm = ConnRectM();
        SkRect rpx = SkRect::MakeLTRB(rm.left / kPxToMetric, -rm.top / kPxToMetric,
                                      rm.right / kPxToMetric, -rm.bottom / kPxToMetric);
        int conn = ui::leptonica::ConnectivityHit(rpx, pp);
        if (conn) {
          if (auto m = LockMorph()) {
            {
              auto lock = std::lock_guard(m->mutex);
              m->connectivity = conn;
            }
            m->WakeToys();
          }
          return std::make_unique<MorphPoke>(p);
        }
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

MorphLevelDrag::MorphLevelDrag(ui::Pointer& p, MorphologyToy& w) : Action(p), widget(&w) {}
void MorphLevelDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = widget->ParamRowM();
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  if (auto m = widget->LockMorph()) {
    {
      auto lock = std::lock_guard(m->mutex);
      if (m->op_mode == 5) {
        m->height = std::clamp(1 + (int)std::lround(t * 254.f), 1, 255);
      } else {
        // Cells are row-major by sel_w: a resize re-strides; the pattern stays centred.
        int n = std::clamp(1 + (int)std::lround(t * (Morphology::kMaxN - 1)), 1, Morphology::kMaxN);
        if (n != m->sel_w || n != m->sel_h) {
          bool uniform = true;
          for (int y = 0; y < m->sel_h && uniform; ++y)
            for (int x = 0; x < m->sel_w && uniform; ++x)
              uniform = m->cells[y * m->sel_w + x] == m->cells[0];
          uint8_t fresh[Morphology::kMaxN * Morphology::kMaxN] = {};
          if (uniform) {
            for (int i = 0; i < n * n; ++i) fresh[i] = m->cells[0];
          } else {
            int dx = (n - m->sel_w) / 2, dy = (n - m->sel_h) / 2;
            for (int y = 0; y < m->sel_h; ++y)
              for (int x = 0; x < m->sel_w; ++x) {
                int nx = x + dx, ny = y + dy;
                if (nx >= 0 && nx < n && ny >= 0 && ny < n)
                  fresh[ny * n + nx] = m->cells[y * m->sel_w + x];
              }
          }
          std::memcpy(m->cells, fresh, sizeof(fresh));
          m->sel_w = n;
          m->sel_h = n;
        }
      }
    }
    m->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Morphology::MakeToy(ui::Widget* parent) {
  return std::make_unique<MorphologyToy>(parent, *this);
}

// ============================================================================
// Tone
// ============================================================================
Pix* Tone::ApplyOp(Pix* in, const float*) const {
  float g;
  int bk, wt;
  bool inv;
  {
    auto lock = std::lock_guard(mutex);
    g = std::max(0.05f, gamma);
    bk = std::clamp(black, 0, 254);
    wt = std::clamp(white, bk + 1, 255);
    inv = invert;
  }
  Pix* out = pixGammaTRC(nullptr, in, g, bk, wt);
  if (!out) return nullptr;
  if (inv) {
    Pix* iv = pixInvert(nullptr, out);
    pixDestroy(&out);
    out = iv;
    if (!out) return nullptr;
  }
  if (pixGetDepth(out) != 32) {
    Pix* o32 = pixConvertTo32(out);
    pixDestroy(&out);
    out = o32;
  }
  return out;
}

void Tone::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("gamma");
  writer.Double(gamma);
  writer.Key("black");
  writer.Int(black);
  writer.Key("white");
  writer.Int(white);
  writer.Key("invert");
  writer.Bool(invert);
}
bool Tone::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "gamma") {
    float v = 0;
    d.Get(v, status);
    if (OK(status)) gamma = v;
  } else if (key == "black") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) black = v;
  } else if (key == "white") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) white = v;
  } else if (key == "invert") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) invert = v;
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct ToneToy;
struct ToneHandleDrag : Action {
  TrackedPtr<ToneToy> widget;
  int which;  // 0 black, 1 white, 2 gamma
  ToneHandleDrag(ui::Pointer& p, ToneToy& w, int which);
  ~ToneHandleDrag();
  void Update() override;
};
struct TonePoke : Action {  // a click that flips the invert toggle
  TonePoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct ToneToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  float gamma = 1.0f;
  int black = 0, white = 255;
  bool invert = false;
  uint8_t lut[256] = {};
  int dragging = -1;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.7_cm;
  constexpr static float kFaceBottom = -1.9_cm;
  constexpr static float kTopEdge = 2.3_cm;
  constexpr static float kTopHump = 4.2_cm;

  Ptr<Tone> LockTone() const { return LockObject<Tone>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  ToneToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockTone()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      gamma = t->gamma;
      black = t->black;
      white = t->white;
      invert = t->invert;
    }
    RebuildLUT();
  }

  bool CenteredAtZero() const override { return true; }

  Rect GraphRectM() const { return Rect(-kHalfW + 0.4_cm, -0.05_cm, -0.7_cm, 2.15_cm); }
  Rect PreviewRectM() const { return Rect(-0.05_cm, -0.05_cm, kHalfW - 0.4_cm, 2.15_cm); }
  float RunCenterY() const { return -1.05_cm; }
  Rect InvertToggleM() const {
    return Rect(-kHalfW + 0.5_cm, -1.42_cm, -kHalfW + 1.9_cm, -0.92_cm);
  }

  void RebuildLUT() {
    int bk = std::clamp(black, 0, 254), wt = std::clamp(white, bk + 1, 255);
    float g = std::max(0.05f, gamma);
    for (int i = 0; i < 256; ++i) {
      float o;
      if (i <= bk)
        o = 0;
      else if (i >= wt)
        o = 255;
      else
        o = 255.f * std::pow((float)(i - bk) / (wt - bk), 1.f / g);
      if (invert) o = 255 - o;
      lut[i] = (uint8_t)std::clamp(o, 0.f, 255.f);
    }
  }

  float InToX(int v) const {
    Rect g = GraphRectM();
    return g.left + g.Width() * std::clamp(v, 0, 255) / 255.f;
  }
  int XToIn(float x) const {
    Rect g = GraphRectM();
    return (int)std::clamp((x - g.left) / g.Width() * 255.f, 0.f, 255.f);
  }
  float OutToY(int v) const {
    Rect g = GraphRectM();
    return g.bottom + g.Height() * std::clamp(v, 0, 255) / 255.f;
  }
  SkPoint HandlePosM(int which) const {
    int in = (which == 0)   ? std::clamp(black, 0, 255)
             : (which == 1) ? std::clamp(white, 0, 255)
                            : (std::clamp(black, 0, 255) + std::clamp(white, 0, 255)) / 2;
    return {InToX(in), OutToY(lut[std::clamp(in, 0, 255)])};
  }
  SkRect GraphPx() const {
    Rect g = GraphRectM();
    return SkRect::MakeLTRB(g.left / kPxToMetric, -g.top / kPxToMetric, g.right / kPxToMetric,
                            -g.bottom / kPxToMetric);
  }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.moveTo(-kHalfW, kFaceBottom);
    b.lineTo(-kHalfW, kTopEdge);
    b.quadTo(0, kTopHump, kHalfW, kTopEdge);
    b.lineTo(kHalfW, kFaceBottom);
    b.close();
    return b.detach();
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kFaceBottom - 0.5_cm, kHalfW + 0.5_cm, kTopHump + 0.5_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 1.0_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t ToneHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockTone()) {
      auto lock = std::lock_guard(t->mutex);
      auto mix = [&](uint32_t v) {
        h ^= v;
        h *= 16777619u;
      };
      uint32_t gb;
      std::memcpy(&gb, &t->gamma, 4);
      mix(gb);
      mix((uint32_t)t->black);
      mix((uint32_t)t->white);
      mix(t->invert ? 1u : 0u);
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scale = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scale)), dh = std::max(1, (int)(sh * scale));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockTone()) {
      uint32_t h = ToneHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        gamma = t->gamma;
        black = t->black;
        white = t->white;
        invert = t->invert;
      }
      RebuildLUT();
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    Rect pr = PreviewRectM();
    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, pr);
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#d8cdbb"_color);
      canvas.drawRect(pr.sk, ph);
      SkPaint key;
      key.setAntiAlias(true);
      key.setStyle(SkPaint::kStroke_Style);
      key.setStrokeWidth(0.3_mm);
      key.setColor("#3a342c"_color);
      canvas.drawRect(pr.sk, key);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto PX = [&](float m) { return m / kPxToMetric; };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xA01, 1);
      DrawDepthChipPx(canvas,
                      SkRect::MakeLTRB(pr.left / kPxToMetric, -pr.top / kPxToMetric,
                                       pr.right / kPxToMetric, -pr.bottom / kPxToMetric),
                      cached_preview, out_depth, out_cmap, 0xA09);

      ui::leptonica::DrawCurveLUT(canvas, GraphPx(), lut, slop::State::Default, 0xA10);
      for (int k = 0; k < 3; ++k) {
        SkPoint hm = HandlePosM(k);
        SkPoint hp = P(hm.fX, hm.fY);
        float r = PX(0.22_cm);
        SkColor col = (k == 2) ? slop::kYellow : (k == 0 ? slop::kInk : slop::kPaper);
        SkPath kn = slop::WobbleEllipse(hp, r, r, slop::kWonk * 0.5f, slop::Hash2(0xA20, k), 18);
        slop::MisregFill(canvas, kn, dragging == k ? slop::kRed : col, slop::Hash2(0xA21, k));
        slop::SketchyStroke(canvas, kn, slop::kInk, slop::kStroke, slop::Hash2(0xA22, k), 1);
      }

      Rect it = InvertToggleM();
      SkRect itpx = SkRect::MakeLTRB(PX(it.left), -it.top / kPxToMetric, PX(it.right),
                                     -it.bottom / kPxToMetric);
      slop::Toggle(canvas, itpx, invert, slop::State::Default, 0xA30);
      slop::DrawText(canvas, "INVERT", P(it.left, it.top + 0.12_cm), 15.f, kLabelInk, false, 0);

      std::string title = "TONE";
      float tcx = 0.55_cm;
      float tpx = slop::TextWidth(title, kTitleTextPx);
      slop::DrawText(canvas, title, P(tcx - tpx * kPxToMetric * 0.5f, RunCenterY() + 0.2_cm),
                     kTitleTextPx, slop::kInk, true, 0xA40);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fpx = slop::TextWidth(credit, kCreditTextPx);
        slop::DrawText(canvas, credit, P(tcx - fpx * kPxToMetric * 0.5f, RunCenterY() - 0.32_cm),
                       kCreditTextPx, "#8a7d66"_color, false, 0xA41);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      for (int k = 0; k < 3; ++k) {
        SkPoint hm = HandlePosM(k);
        if (std::hypot(pos.x - hm.fX, pos.y - hm.fY) <= 0.4_cm)
          return std::make_unique<ToneHandleDrag>(p, *this, k);
      }
      Rect it = InvertToggleM();
      if (pos.x >= it.left && pos.x <= it.right && pos.y >= it.bottom && pos.y <= it.top) {
        if (auto t = LockTone()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->invert = !t->invert;
          }
          t->WakeToys();
        }
        return std::make_unique<TonePoke>(p);
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

ToneHandleDrag::ToneHandleDrag(ui::Pointer& p, ToneToy& w, int which)
    : Action(p), widget(&w), which(which) {
  if (widget) widget->dragging = which;
}
ToneHandleDrag::~ToneHandleDrag() {
  if (widget) {
    widget->dragging = -1;
    widget->WakeAnimation();
  }
}
void ToneHandleDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  if (auto t = widget->LockTone()) {
    Rect g = widget->GraphRectM();
    {
      auto lock = std::lock_guard(t->mutex);
      if (which == 0) {
        t->black = std::clamp(widget->XToIn(pos.x), 0, t->white - 2);
      } else if (which == 1) {
        t->white = std::clamp(widget->XToIn(pos.x), t->black + 2, 255);
      } else {
        float outNorm = std::clamp((pos.y - g.bottom) / std::max(1e-4f, g.Height()), 0.02f, 0.98f);
        if (t->invert) outNorm = 1.f - outNorm;
        outNorm = std::clamp(outNorm, 0.02f, 0.98f);
        t->gamma = std::clamp(std::log(0.5f) / std::log(outNorm), 0.1f, 6.0f);
      }
    }
    t->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Tone::MakeToy(ui::Widget* parent) {
  return std::make_unique<ToneToy>(parent, *this);
}

// ============================================================================
// Geometry
// ============================================================================
bool Geometry::DrivenAngle(float& out) const {
  if (auto* src = angle_src->ObjectOrNull()) {
    auto txt = src->GetText();
    if (!txt.empty()) {
      char* end = nullptr;
      double v = strtod(txt.c_str(), &end);
      if (end != txt.c_str()) {
        out = std::clamp((float)v, -360.f, 360.f);
        return true;
      }
    }
  }
  return false;
}

Pix* Geometry::ApplyOp(Pix* in, const float*) const {
  float ang, sc, sy;
  bool mir, flp, px, abs_mode, lock;
  int tw, th;
  {
    auto lock_ = std::lock_guard(mutex);
    ang = angle_deg;
    sc = std::clamp(scale, 0.05f, 16.f);
    sy = lock_aspect ? sc : std::clamp(scale_y, 0.05f, 16.f);
    abs_mode = absolute;
    lock = lock_aspect;
    tw = std::clamp(target_w, 8, 4000);
    th = std::clamp(target_h, 8, 4000);
    mir = mirror;
    flp = flip;
    px = pixels;
  }
  float driven_ang;
  if (DrivenAngle(driven_ang)) ang = driven_ang;  // the Angle port overrides the dial
  Pix* cur = pixCopy(nullptr, in);
  if (!cur) return nullptr;
  if (abs_mode) {
    // SIZE: scale to the exact pixel target (height derived while the aspect is locked)
    int w = pixGetWidth(cur), h = pixGetHeight(cur);
    Pix* s;
    if (px && w > 0 && h > 0) {
      float sx = (float)tw / w;
      float sy2 = lock ? sx : (float)th / h;
      s = pixScaleBySampling(cur, sx, sy2);
    } else {
      s = pixScaleToSize(cur, tw, lock ? 0 : th);
    }
    pixDestroy(&cur);
    cur = s;
    if (!cur) return nullptr;
  } else if (std::abs(sc - 1.f) > 0.01f || std::abs(sy - 1.f) > 0.01f) {
    Pix* s = px ? pixScaleBySampling(cur, sc, sy) : pixScale(cur, sc, sy);
    pixDestroy(&cur);
    cur = s;
    if (!cur) return nullptr;
  }
  float a = std::fmod(ang, 360.f);
  if (std::abs(a) > 0.2f && std::abs(a - 360.f) > 0.2f) {
    float rad = a * 3.14159265f / 180.f;
    int w = pixGetWidth(cur), h = pixGetHeight(cur);
    Pix* r = pixRotate(cur, rad, L_ROTATE_AREA_MAP, L_BRING_IN_WHITE, w, h);
    pixDestroy(&cur);
    cur = r;
    if (!cur) return nullptr;
  }
  if (mir && cur) pixFlipLR(cur, cur);
  if (flp && cur) pixFlipTB(cur, cur);
  if (cur && pixGetDepth(cur) != 32) {
    Pix* c = pixConvertTo32(cur);
    pixDestroy(&cur);
    cur = c;
  }
  return cur;
}
void Geometry::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("angle");
  writer.Double(angle_deg);
  writer.Key("scale");
  writer.Double(scale);
  writer.Key("absolute");
  writer.Bool(absolute);
  writer.Key("target_w");
  writer.Int(target_w);
  writer.Key("target_h");
  writer.Int(target_h);
  writer.Key("scale_y");
  writer.Double(scale_y);
  writer.Key("lock_aspect");
  writer.Bool(lock_aspect);
  writer.Key("pixels");
  writer.Bool(pixels);
  writer.Key("mirror");
  writer.Bool(mirror);
  writer.Key("flip");
  writer.Bool(flip);
}
bool Geometry::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "angle") {
    float v = 0;
    d.Get(v, status);
    if (OK(status)) angle_deg = v;
  } else if (key == "scale") {
    float v = 0;
    d.Get(v, status);
    if (OK(status)) scale = v;
  } else if (key == "absolute") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) absolute = v;
  } else if (key == "target_w") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) target_w = std::clamp(v, 8, 4000);
  } else if (key == "target_h") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) target_h = std::clamp(v, 8, 4000);
  } else if (key == "scale_y") {
    float v = 0;
    d.Get(v, status);
    if (OK(status)) scale_y = std::clamp(v, 0.05f, 16.f);
  } else if (key == "lock_aspect") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) lock_aspect = v;
  } else if (key == "pixels") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) pixels = v;
  } else if (key == "mirror") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) mirror = v;
  } else if (key == "flip") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) flip = v;
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct GeometryToy;
struct GeoRingDrag : Action {
  TrackedPtr<GeometryToy> widget;
  GeoRingDrag(ui::Pointer& p, GeometryToy& w);
  ~GeoRingDrag();
  void Update() override;
};
struct GeoCornerDrag : Action {
  TrackedPtr<GeometryToy> widget;
  float start_dist = 1.f, start_scale = 1.f;
  GeoCornerDrag(ui::Pointer& p, GeometryToy& w);
  ~GeoCornerDrag();
  void Update() override;
};
struct GeoDialDrag : Action {
  TrackedPtr<GeometryToy> widget;
  GeoDialDrag(ui::Pointer& p, GeometryToy& w);
  ~GeoDialDrag();
  void Update() override;
};
struct GeoScaleDrag : Action {
  TrackedPtr<GeometryToy> widget;
  GeoScaleDrag(ui::Pointer& p, GeometryToy& w);
  ~GeoScaleDrag();
  void Update() override;
};

struct GeoScaleYDrag : Action {
  TrackedPtr<GeometryToy> widget;
  GeoScaleYDrag(ui::Pointer& p, GeometryToy& w);
  ~GeoScaleYDrag();
  void Update() override;
};
struct GeoTogglePoke : Action {
  GeoTogglePoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct GeometryToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  float angle_deg = 0.f;
  bool angle_driven = false;
  float scale = 1.0f;
  float scale_y = 1.0f;
  bool absolute = false;
  int target_w = 800;
  int target_h = 600;
  bool lock_aspect = true;
  bool pixels = false;
  bool mirror = false;
  bool flip = false;
  int dragging = 0;  // 0 none, 1 dial, 2 scale slider, 3 on-image ring, 4 corner grip

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.6_cm;
  constexpr static float kFaceTop = 2.85_cm;
  constexpr static float kFaceBottom = -3.2_cm;
  constexpr static float kDialCX = -2.0_cm;
  constexpr static float kDialCY = 1.3_cm;
  constexpr static float kDialR = 1.35_cm;

  Ptr<Geometry> LockGeo() const { return LockObject<Geometry>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  GeometryToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockGeo()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      angle_deg = t->angle_deg;
      scale = t->scale;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewRectM() const { return Rect(-0.05_cm, 0.0_cm, kHalfW - 0.35_cm, 2.4_cm); }
  Rect ScaleSliderM() const { return Rect(-1.85_cm, -0.7_cm, 1.0_cm, -0.25_cm); }
  Rect ScaleYSliderM() const { return Rect(-1.85_cm, -1.06_cm, 1.0_cm, -0.78_cm); }
  Rect MirrorToggleM() const { return Rect(-1.85_cm, -1.52_cm, -0.95_cm, -1.16_cm); }
  Rect FlipToggleM() const { return Rect(-0.75_cm, -1.52_cm, 0.15_cm, -1.16_cm); }
  Rect LockToggleM() const { return Rect(0.35_cm, -1.52_cm, 1.25_cm, -1.16_cm); }
  Rect PixelsToggleM() const { return Rect(1.15_cm, -1.04_cm, 1.95_cm, -0.72_cm); }
  Rect SizeToggleM() const { return Rect(1.15_cm, -0.68_cm, 1.95_cm, -0.36_cm); }
  // px target <-> slider t (log 32..3200)
  static float PxToT(int v) { return std::clamp(std::log(v / 32.f) / std::log(100.f), 0.f, 1.f); }
  static int TToPx(float t) {
    return (int)std::lround(32.f * std::pow(100.f, std::clamp(t, 0.f, 1.f)));
  }
  float ScaleYToT() const {
    return std::clamp(std::log(scale_y / 0.25f) / std::log(16.f), 0.f, 1.f);
  }
  Vec2 RingCenterM() const {
    Rect p = PreviewRectM();
    return {p.CenterX(), p.CenterY()};
  }
  float RingRadiusM() const {
    Rect p = PreviewRectM();
    return std::min(p.Width(), p.Height()) * 0.5f + 0.18_cm;
  }
  Rect FittedRectM() const {
    Rect area = PreviewRectM();
    if (!cached_preview) return area;
    float iw = (float)cached_preview->width(), ih = (float)cached_preview->height();
    if (iw <= 0 || ih <= 0) return area;
    float s = std::min(area.Width() / iw, area.Height() / ih);
    return Rect::MakeCenter({area.CenterX(), area.CenterY()}, iw * s, ih * s);
  }
  SkPoint DialCenterPx() const { return {kDialCX / kPxToMetric, -kDialCY / kPxToMetric}; }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRRect(RRect::MakeSimple(Rect(kDialCX, kFaceBottom, kHalfW, kFaceTop), 4_mm).sk);
    b.addOval(SkRect::MakeXYWH(kDialCX - 1.55_cm, kDialCY - 1.55_cm, 3.1_cm, 3.1_cm));
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::RRect(RRect::MakeSimple(Rect(kDialCX, kFaceBottom, kHalfW, kFaceTop), 4_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(kDialCX - 2.1_cm, kFaceBottom - 1.2_cm, kHalfW + 0.5_cm, kFaceTop + 0.5_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {kHalfW, 1.0_cm}, .dir = 0_deg};
    if (&arg == static_cast<const Interface::Table*>(&Geometry::angle_src_tbl))
      return {.pos = {kDialCX - 1.55_cm, kDialCY}, .dir = 180_deg};  // into the dial's left edge
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  // scale <-> slider t (log: t=0.5 -> 1.0x, range 0.25..4)
  float ScaleToT() const { return std::clamp(std::log(scale / 0.25f) / std::log(16.f), 0.f, 1.f); }
  float TToScale(float t) const { return 0.25f * std::pow(16.f, std::clamp(t, 0.f, 1.f)); }

  uint32_t GeoHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockGeo()) {
      auto lock = std::lock_guard(t->mutex);
      uint32_t a, s;
      std::memcpy(&a, &t->angle_deg, 4);
      std::memcpy(&s, &t->scale, 4);
      a ^= (t->mirror ? 0x40000000u : 0u) ^ (t->flip ? 0x10000000u : 0u) ^
           (t->lock_aspect ? 0x04000000u : 0u) ^ (t->pixels ? 0x01000000u : 0u) ^
           (t->absolute ? 0x00400000u : 0u);
      a ^= (uint32_t)t->target_w * 2246822519u;
      a ^= (uint32_t)t->target_h * 3266489917u;
      uint32_t sy_bits;
      std::memcpy(&sy_bits, &t->scale_y, 4);
      s ^= sy_bits * 2654435761u;
      h ^= a;
      h *= 16777619u;
      h ^= s;
      h *= 16777619u;
      float dv;
      if (t->DrivenAngle(dv)) {
        h ^= (uint32_t)(int)std::lround(dv * 100.f) * 2246822519u;
        h ^= 0x5EED5EEDu;
      }
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockGeo()) {
      uint32_t h = GeoHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        angle_deg = t->angle_deg;
        scale = t->scale;
        scale_y = t->scale_y;
        absolute = t->absolute;
        target_w = t->target_w;
        target_h = t->target_h;
        lock_aspect = t->lock_aspect;
        pixels = t->pixels;
        mirror = t->mirror;
        flip = t->flip;
      }
      float dv;
      if (t->DrivenAngle(dv)) {
        angle_driven = true;
        angle_deg = dv;
      } else {
        angle_driven = false;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    Rect pr = PreviewRectM();
    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, pr);
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#d8cdbb"_color);
      canvas.drawRect(pr.sk, ph);
      SkPaint key;
      key.setAntiAlias(true);
      key.setStyle(SkPaint::kStroke_Style);
      key.setStrokeWidth(0.3_mm);
      key.setColor("#3a342c"_color);
      canvas.drawRect(pr.sk, key);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX2 = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      auto PX = [&](float m) { return m / kPxToMetric; };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xB01, 1);
      DrawDepthChipPx(canvas,
                      SkRect::MakeLTRB(pr.left / kPxToMetric, -pr.top / kPxToMetric,
                                       pr.right / kPxToMetric, -pr.bottom / kPxToMetric),
                      cached_preview, out_depth, out_cmap, 0xB09);

      ui::leptonica::DrawDial(canvas, DialCenterPx(), PX(kDialR), angle_deg,
                              angle_driven ? slop::State::Disabled : slop::State::Default, 0xB10);

      {
        Vec2 rc = RingCenterM();
        SkPoint cpx{rc.x / kPxToMetric, -rc.y / kPxToMetric};
        ui::leptonica::DrawTransformRing(
            canvas, cpx, RingRadiusM() / kPxToMetric, angle_deg,
            dragging == 3 ? slop::State::Pressed : slop::State::Default, 0xB60);
        if (cached_preview) {
          Rect fit = FittedRectM();
          SkPoint corners[4] = {{fit.left, fit.top},
                                {fit.right, fit.top},
                                {fit.left, fit.bottom},
                                {fit.right, fit.bottom}};
          for (int i2 = 0; i2 < 4; ++i2) {
            SkPoint cp2{corners[i2].fX / kPxToMetric, -corners[i2].fY / kPxToMetric};
            SkRect g = SkRect::MakeXYWH(cp2.fX - 6.f, cp2.fY - 6.f, 12.f, 12.f);
            SkPath gp = slop::WonkyRoundRect(g, 2.5f, slop::kWonk * 0.5f, 0xB70u + (uint32_t)i2);
            slop::MisregFill(canvas, gp, dragging == 4 ? slop::kRed : slop::kYellow,
                             0xB74u + (uint32_t)i2);
            slop::SketchyStroke(canvas, gp, slop::kInk, slop::kStrokeHair, 0xB78u + (uint32_t)i2,
                                1);
          }
        }
      }
      {
        char buf[20];
        if (angle_driven)
          snprintf(buf, sizeof(buf), "%d\xC2\xB0 DRIVEN", ((int)std::lround(angle_deg)) % 360);
        else
          snprintf(buf, sizeof(buf), "%d\xC2\xB0", ((int)std::lround(angle_deg)) % 360);
        float fs = 22.f, tw = slop::TextWidth(buf, fs), cw = tw + PX(0.3_cm), ch = fs * 1.5f;
        SkPoint cc = P(kDialCX, kDialCY - kDialR - 0.28_cm);
        SkRect chip = SkRect::MakeXYWH(cc.fX - cw / 2, cc.fY - ch / 2, cw, ch);
        SkPath cp = slop::WonkyRoundRect(chip, ch * 0.35f, slop::kWonk, 0xB21);
        slop::MisregFill(canvas, cp, slop::kPaper, 0xB22);
        slop::SketchyStroke(canvas, cp, slop::kInk, slop::kStroke, 0xB23, 1);
        slop::DrawText(canvas, buf, {cc.fX - tw / 2, cc.fY + fs * 0.36f}, fs, slop::kInk, false, 0);
      }

      Rect ss = ScaleSliderM();
      SkRect sspx = SkRect::MakeLTRB(PX(ss.left), -ss.top / kPxToMetric, PX(ss.right),
                                     -ss.bottom / kPxToMetric);
      slop::Slider(canvas, sspx, absolute ? PxToT(target_w) : ScaleToT(), slop::State::Default,
                   0xB30);
      {
        char buf[16];
        if (absolute)
          snprintf(buf, sizeof(buf), "W %d px", target_w);
        else
          snprintf(buf, sizeof(buf), "SCALE %.2fx", scale);
        slop::DrawText(canvas, buf, P(ss.left, ss.top + 0.12_cm), 15.f, kLabelInk, false, 0);
      }

      {
        Rect sy = ScaleYSliderM();
        float t2 = absolute ? PxToT(target_h) : (lock_aspect ? ScaleToT() : ScaleYToT());
        slop::Slider(canvas, RPX2(sy), t2,
                     lock_aspect ? slop::State::Disabled : slop::State::Default, 0xB85);
        char buf2[20];
        if (absolute) {
          if (lock_aspect)
            snprintf(buf2, sizeof(buf2), "H auto");
          else
            snprintf(buf2, sizeof(buf2), "H %d px", target_h);
        } else {
          snprintf(buf2, sizeof(buf2), "SCALE Y %.2fx", lock_aspect ? scale : scale_y);
        }
        slop::DrawText(canvas, buf2, P(sy.left, sy.top + 0.1_cm), 13.f, kLabelInk, false, 0);
      }

      slop::Toggle(canvas, RPX2(MirrorToggleM()), mirror, slop::State::Default, 0xB80);
      slop::DrawText(canvas, "MIRROR", P(MirrorToggleM().left, MirrorToggleM().top + 0.12_cm), 13.f,
                     kLabelInk, false, 0);
      slop::Toggle(canvas, RPX2(FlipToggleM()), flip, slop::State::Default, 0xB81);
      slop::DrawText(canvas, "FLIP", P(FlipToggleM().left, FlipToggleM().top + 0.12_cm), 13.f,
                     kLabelInk, false, 0);
      slop::Toggle(canvas, RPX2(LockToggleM()), lock_aspect, slop::State::Default, 0xB82);
      slop::DrawText(canvas, "LOCK", P(LockToggleM().left, LockToggleM().top + 0.12_cm), 13.f,
                     kLabelInk, false, 0);
      slop::Toggle(canvas, RPX2(PixelsToggleM()), pixels, slop::State::Default, 0xB83);
      slop::DrawText(canvas, "PIXELS", P(PixelsToggleM().left, PixelsToggleM().top + 0.12_cm), 13.f,
                     kLabelInk, false, 0);
      slop::Toggle(canvas, RPX2(SizeToggleM()), absolute, slop::State::Default, 0xB84);
      slop::DrawText(canvas, "W\xc2\xb7H", P(SizeToggleM().left, SizeToggleM().top + 0.12_cm), 13.f,
                     kLabelInk, false, 0);

      std::string title = "GEOMETRY";
      float tcx = -0.3_cm;
      float tpx = slop::TextWidth(title, kTitleTextPx);
      slop::DrawText(canvas, title, P(tcx - tpx * kPxToMetric * 0.5f, -2.0_cm), kTitleTextPx,
                     slop::kInk, true, 0xB40);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fpx = slop::TextWidth(credit, kCreditTextPx);
        slop::DrawText(canvas, credit, P(tcx - fpx * kPxToMetric * 0.5f, -2.38_cm), kCreditTextPx,
                       "#8a7d66"_color, false, 0xB41);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      if (cached_preview && !absolute) {
        Rect fit = FittedRectM();
        SkPoint corners[4] = {{fit.left, fit.top},
                              {fit.right, fit.top},
                              {fit.left, fit.bottom},
                              {fit.right, fit.bottom}};
        for (int i2 = 0; i2 < 4; ++i2) {
          if (std::hypot(pos.x - corners[i2].fX, pos.y - corners[i2].fY) <= 0.28_cm)
            return std::make_unique<GeoCornerDrag>(p, *this);
        }
      }
      if (!angle_driven) {
        Vec2 rc = RingCenterM();
        if (ui::leptonica::TransformRingHit({rc.x, rc.y}, RingRadiusM(), {pos.x, pos.y}, 0.16_cm))
          return std::make_unique<GeoRingDrag>(p, *this);
      }
      float dd = std::hypot(pos.x - kDialCX, pos.y - kDialCY);
      if (!angle_driven && dd <= kDialR + 0.2_cm && dd >= kDialR * 0.2f)
        return std::make_unique<GeoDialDrag>(p, *this);
      Rect ss = ScaleSliderM();
      if (pos.x >= ss.left - 0.2_cm && pos.x <= ss.right + 0.2_cm && pos.y >= ss.bottom - 0.2_cm &&
          pos.y <= ss.top + 0.2_cm)
        return std::make_unique<GeoScaleDrag>(p, *this);
      {
        Rect sy = ScaleYSliderM();
        if (!lock_aspect && pos.x >= sy.left - 0.2_cm && pos.x <= sy.right + 0.2_cm &&
            pos.y >= sy.bottom - 0.15_cm && pos.y <= sy.top + 0.15_cm)
          return std::make_unique<GeoScaleYDrag>(p, *this);
      }
      for (int which = 0; which < 5; ++which) {
        Rect r = which == 0   ? MirrorToggleM()
                 : which == 1 ? FlipToggleM()
                 : which == 2 ? LockToggleM()
                 : which == 3 ? PixelsToggleM()
                              : SizeToggleM();
        if (pos.x >= r.left - 0.1_cm && pos.x <= r.right + 0.1_cm && pos.y >= r.bottom - 0.1_cm &&
            pos.y <= r.top + 0.1_cm) {
          if (auto t = LockGeo()) {
            {
              auto lock = std::lock_guard(t->mutex);
              if (which == 0)
                t->mirror = !t->mirror;
              else if (which == 1)
                t->flip = !t->flip;
              else if (which == 2)
                t->lock_aspect = !t->lock_aspect;
              else if (which == 3)
                t->pixels = !t->pixels;
              else
                t->absolute = !t->absolute;
            }
            t->WakeToys();
          }
          return std::make_unique<GeoTogglePoke>(p);
        }
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

GeoDialDrag::GeoDialDrag(ui::Pointer& p, GeometryToy& w) : Action(p), widget(&w) {
  if (widget) widget->dragging = 1;
}
GeoDialDrag::~GeoDialDrag() {
  if (widget) {
    widget->dragging = 0;
    widget->WakeAnimation();
  }
}
void GeoDialDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  // DialAngleAt is pixel-space (y-down); we are metric (y-up). Negate y or the
  // needle mirrors the mouse.
  SkPoint c = {GeometryToy::kDialCX, -GeometryToy::kDialCY};
  float ang = ui::leptonica::DialAngleAt(c, {pos.x, -pos.y});
  if (auto t = widget->LockGeo()) {
    {
      auto lock = std::lock_guard(t->mutex);
      t->angle_deg = ang;
    }
    t->WakeToys();
  }
  widget->WakeAnimation();
}

GeoScaleDrag::GeoScaleDrag(ui::Pointer& p, GeometryToy& w) : Action(p), widget(&w) {
  if (widget) widget->dragging = 2;
}
GeoScaleDrag::~GeoScaleDrag() {
  if (widget) {
    widget->dragging = 0;
    widget->WakeAnimation();
  }
}
void GeoScaleDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect ss = widget->ScaleSliderM();
  float t = std::clamp((pos.x - ss.left) / std::max(1e-4f, ss.Width()), 0.f, 1.f);
  if (auto g = widget->LockGeo()) {
    {
      auto lock = std::lock_guard(g->mutex);
      if (widget->absolute)
        g->target_w = GeometryToy::TToPx(t);
      else
        g->scale = widget->TToScale(t);
    }
    g->WakeToys();
  }
  widget->WakeAnimation();
}

GeoRingDrag::GeoRingDrag(ui::Pointer& p, GeometryToy& w) : Action(p), widget(&w) {
  if (widget) widget->dragging = 3;
}
GeoRingDrag::~GeoRingDrag() {
  if (widget) {
    widget->dragging = 0;
    widget->WakeAnimation();
  }
}
void GeoRingDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Vec2 rc = widget->RingCenterM();
  // TransformRingAngleAt is pixel-space (y-down); negate y of the metric coords.
  float ang = ui::leptonica::TransformRingAngleAt({rc.x, -rc.y}, {pos.x, -pos.y});
  if (auto t = widget->LockGeo()) {
    {
      auto lock = std::lock_guard(t->mutex);
      t->angle_deg = ang;
    }
    t->WakeToys();
  }
  widget->WakeAnimation();
}

GeoCornerDrag::GeoCornerDrag(ui::Pointer& p, GeometryToy& w) : Action(p), widget(&w) {
  widget->dragging = 4;
  Vec2 pos = p.PositionWithin(w);
  Vec2 rc = w.RingCenterM();
  start_dist = std::max(0.05f, std::hypot(pos.x - rc.x, pos.y - rc.y));
  start_scale = w.scale;
}
GeoCornerDrag::~GeoCornerDrag() {
  if (widget) {
    widget->dragging = 0;
    widget->WakeAnimation();
  }
}
void GeoCornerDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Vec2 rc = widget->RingCenterM();
  float d = std::max(0.05f, std::hypot(pos.x - rc.x, pos.y - rc.y));
  float ns = std::clamp(start_scale * d / start_dist, 0.25f, 4.f);
  if (auto t = widget->LockGeo()) {
    {
      auto lock = std::lock_guard(t->mutex);
      t->scale = ns;
    }
    t->WakeToys();
  }
  widget->WakeAnimation();
}

GeoScaleYDrag::GeoScaleYDrag(ui::Pointer& p, GeometryToy& w) : Action(p), widget(&w) {
  if (widget) widget->dragging = 2;
}
GeoScaleYDrag::~GeoScaleYDrag() {
  if (widget) {
    widget->dragging = 0;
    widget->WakeAnimation();
  }
}
void GeoScaleYDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = widget->ScaleYSliderM();
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  float ns = 0.25f * std::pow(16.f, t);
  if (auto g = widget->LockGeo()) {
    {
      auto lock = std::lock_guard(g->mutex);
      if (widget->absolute)
        g->target_h = GeometryToy::TToPx(t);
      else
        g->scale_y = std::clamp(ns, 0.25f, 4.f);
    }
    g->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Geometry::MakeToy(ui::Widget* parent) {
  return std::make_unique<GeometryToy>(parent, *this);
}

// ============================================================================
// Channel
// ============================================================================
Pix* Channel::ApplyOp(Pix* in, const float*) const {
  int ch;
  float r, g, b;
  {
    auto lock = std::lock_guard(mutex);
    ch = std::clamp(channel, 0, 7);
    r = std::clamp(wr, 0.f, 1.f);
    g = std::clamp(wg, 0.f, 1.f);
    b = std::clamp(wb, 0.f, 1.f);
  }
  Pix* gray;
  if (ch == 0) {
    gray = pixConvertRGBToLuminance(in);
  } else if (ch <= 3) {
    gray = pixGetRGBComponent(in, ch - 1);
  } else if (ch <= 6) {
    static constexpr int kMinMax[] = {L_CHOOSE_MIN, L_CHOOSE_MAX, L_CHOOSE_MAXDIFF};
    gray = pixConvertRGBToGrayMinMax(in, kMinMax[ch - 4]);
  } else {
    if (r + g + b <= 0.01f) {
      r = 0.30f;
      g = 0.59f;
      b = 0.11f;
    }
    gray = pixConvertRGBToGray(in, r, g, b);  // the 3-pipe MIX
  }
  return gray;  // native 8 bpp - preview + surface upconvert
}
void Channel::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("channel");
  writer.Int(channel);
  writer.Key("wr");
  writer.Double(wr);
  writer.Key("wg");
  writer.Double(wg);
  writer.Key("wb");
  writer.Double(wb);
}
bool Channel::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "channel") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) channel = v;
  } else if (key == "wr" || key == "wg" || key == "wb") {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) {
      float v = std::clamp((float)x, 0.f, 1.f);
      if (key == "wr")
        wr = v;
      else if (key == "wg")
        wg = v;
      else
        wb = v;
    }
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct ChannelToy;
struct ChannelPoke : Action {
  ChannelPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct ChannelToy;
struct ChannelWeightDrag : Action {
  TrackedPtr<ChannelToy> widget;
  int which;
  ChannelWeightDrag(ui::Pointer& p, ChannelToy& w, int which);
  ~ChannelWeightDrag();
  void Update() override;
};

struct ChannelToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int channel = 0;
  float wr = 0.30f, wg = 0.59f, wb = 0.11f;
  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.0_cm;
  constexpr static float kFaceTop = 2.3_cm;
  constexpr static float kFaceBottom = -2.0_cm;
  constexpr static float kPeakTopX = 0.2_cm;
  constexpr static float kPeakTopY = 3.35_cm;  // kFaceTop + ~1.05

  Ptr<Channel> LockChannel() const { return LockObject<Channel>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  ChannelToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockChannel()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      channel = t->channel;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect TapRectM() const { return Rect(-kHalfW + 0.35_cm, 1.35_cm, kHalfW - 0.35_cm, 2.0_cm); }
  Rect WeightSliderM(int i) const {
    float top = -1.34_cm - i * 0.22_cm;
    return Rect(-2.2_cm, top - 0.16_cm, -0.95_cm, top);
  }
  Rect PreviewRectM() const { return Rect(-kHalfW + 0.35_cm, -1.25_cm, kHalfW - 0.35_cm, 1.15_cm); }
  SkRect TapRectPx() const {
    Rect t = TapRectM();
    return SkRect::MakeLTRB(t.left / kPxToMetric, -t.top / kPxToMetric, t.right / kPxToMetric,
                            -t.bottom / kPxToMetric);
  }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRRect(RRect::MakeSimple(Rect(-kHalfW, kFaceBottom, kHalfW, kFaceTop), 4_mm).sk);
    SkPath peak = SkPathBuilder()
                      .moveTo(-1.0_cm, kFaceTop - 0.05_cm)
                      .lineTo(kPeakTopX, kPeakTopY)
                      .lineTo(1.4_cm, kFaceTop - 0.05_cm)
                      .close()
                      .detach();
    b.addPath(peak);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::RRect(RRect::MakeSimple(Rect(-kHalfW, kFaceBottom, kHalfW, kFaceTop), 4_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kFaceBottom - 1.0_cm, kHalfW + 0.5_cm, kPeakTopY + 0.7_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl)) {
      Rect p = PreviewRectM();
      return {.pos = {-kHalfW, p.CenterY()}, .dir = 180_deg};
    }
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t ChanHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockChannel()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)t->channel;
      h *= 16777619u;
      for (float f : {t->wr, t->wg, t->wb}) {
        h ^= (uint32_t)std::lround(f * 512.f);
        h *= 16777619u;
      }
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockChannel()) {
      uint32_t h = ChanHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        channel = t->channel;
        wr = t->wr;
        wg = t->wg;
        wb = t->wb;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    Rect pr = PreviewRectM();
    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, pr);
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#d8cdbb"_color);
      canvas.drawRect(pr.sk, ph);
      SkPaint key;
      key.setAntiAlias(true);
      key.setStyle(SkPaint::kStroke_Style);
      key.setStrokeWidth(0.3_mm);
      key.setColor("#3a342c"_color);
      canvas.drawRect(pr.sk, key);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xC01, 1);
      DrawDepthChipPx(canvas,
                      SkRect::MakeLTRB(pr.left / kPxToMetric, -pr.top / kPxToMetric,
                                       pr.right / kPxToMetric, -pr.bottom / kPxToMetric),
                      cached_preview, out_depth, out_cmap, 0xC09);

      SkColor rays[3] = {slop::kRed, slop::kGreen, slop::kBlue};
      float ang[3] = {-0.5f, 0.0f, 0.5f};
      for (int i = 0; i < 3; ++i) {
        SkPoint a = P(kPeakTopX, kPeakTopY);
        SkPoint b = P(kPeakTopX + ang[i] * 0.9_cm, kPeakTopY + 0.6_cm);
        canvas.drawPath(
            slop::WobbleLine(a, b, slop::kWonk * 0.5f, slop::kSeg, slop::Hash2(0xC10, i)),
            slop::InkPaint(rays[i], slop::kStrokeBold));
      }

      static const char* const kTapLabs[8] = {"LUM", "R", "G", "B", "MIN", "MAX", "DIF", "MIX"};
      static constexpr SkColor kTapCols[8] = {0xff8a8a8a, slop::kRed, slop::kGreen,  slop::kBlue,
                                              0xff404040, 0xffd9d9d9, slop::kPurple, slop::kOrange};
      ui::leptonica::DrawChannelTap(canvas, TapRectPx(), channel, slop::State::Default, 0xC20,
                                    kTapLabs, kTapCols, 8);

      if (channel == 7) {
        static const char* const kW[3] = {"R", "G", "B"};
        static const SkColor kWc[3] = {0xffed1c24, 0xff22b14c, 0xff3f48cc};
        const float ws[3] = {wr, wg, wb};
        for (int i = 0; i < 3; ++i) {
          Rect s = WeightSliderM(i);
          SkRect spx = SkRect::MakeLTRB(s.left / kPxToMetric, -s.top / kPxToMetric,
                                        s.right / kPxToMetric, -s.bottom / kPxToMetric);
          slop::Slider(canvas, spx, std::clamp(ws[i], 0.f, 1.f), slop::State::Default,
                       0xC40u + (uint32_t)i);
          char buf[16];
          snprintf(buf, sizeof(buf), "%s %.2f", kW[i], ws[i]);
          slop::DrawText(canvas, buf, {spx.fLeft - 52.f, spx.fBottom - 2.f}, 12.f, kWc[i], false,
                         0);
        }
      }

      std::string title = "CHANNEL";
      float tcx = -0.3_cm;
      float tpx = slop::TextWidth(title, kTitleTextPx);
      slop::DrawText(canvas, title, P(tcx - tpx * kPxToMetric * 0.5f, -1.5_cm), kTitleTextPx,
                     slop::kInk, true, 0xC30);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fpx = slop::TextWidth(credit, kCreditTextPx);
        slop::DrawText(canvas, credit, P(tcx - fpx * kPxToMetric * 0.5f, -1.92_cm), kCreditTextPx,
                       "#8a7d66"_color, false, 0xC31);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint pp = {pos.x / kPxToMetric, -pos.y / kPxToMetric};
      if (channel == 7) {
        for (int i = 0; i < 3; ++i) {
          Rect s = WeightSliderM(i);
          if (pos.x >= s.left - 0.2_cm && pos.x <= s.right + 0.2_cm &&
              pos.y >= s.bottom - 0.08_cm && pos.y <= s.top + 0.08_cm)
            return std::make_unique<ChannelWeightDrag>(p, *this, i);
        }
      }
      int ch = ui::leptonica::ChannelTapAt(TapRectPx(), pp, 8);
      if (ch >= 0) {
        if (auto t = LockChannel()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->channel = ch;
          }
          t->WakeToys();
        }
        return std::make_unique<ChannelPoke>(p);
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

ChannelWeightDrag::ChannelWeightDrag(ui::Pointer& p, ChannelToy& w, int which)
    : Action(p), widget(&w), which(which) {}
ChannelWeightDrag::~ChannelWeightDrag() {
  if (widget) widget->WakeAnimation();
}
void ChannelWeightDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = widget->WeightSliderM(which);
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  if (auto c = widget->LockObject<Channel>()) {
    {
      auto lock = std::lock_guard(c->mutex);
      if (which == 0)
        c->wr = t;
      else if (which == 1)
        c->wg = t;
      else
        c->wb = t;
    }
    c->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Channel::MakeToy(ui::Widget* parent) {
  return std::make_unique<ChannelToy>(parent, *this);
}

// ============================================================================
// Convolve
// ============================================================================
bool Convolve::DrivenRadius(float& out) const {
  if (auto* src = radius_src->ObjectOrNull()) {
    auto txt = src->GetText();
    if (!txt.empty()) {
      char* end = nullptr;
      double v = strtod(txt.c_str(), &end);
      if (end != txt.c_str()) {
        out = std::clamp((float)v, 0.f, 20.f);
        return true;
      }
    }
  }
  return false;
}

Pix* Convolve::ApplyOp(Pix* in, const float*) const {
  int m, r;
  float amt, rk;
  {
    auto lock = std::lock_guard(mutex);
    m = mode;
    r = std::clamp(radius, 0, 20);
    amt = std::clamp(amount, 0.f, 1.f);
    rk = std::clamp(rank, 0.f, 1.f);
  }
  float driven_r;
  if (DrivenRadius(driven_r)) r = (int)std::lround(driven_r);  // the Radius port wins
  Pix* out = nullptr;
  if (m == 1) {
    out = pixUnsharpMasking(in, std::max(1, r), amt);
  } else if (m == 2) {
    Pix* gray = pixConvertTo8(in, 0);
    if (!gray) return nullptr;
    out = pixSobelEdgeFilter(gray, L_ALL_EDGES);
    pixDestroy(&gray);
  } else if (m == 3) {
    // BAND: edges as the difference of two smoothings; the radius sets the band's centre.
    int s1 = std::clamp(r, 1, 4);
    out = pixHalfEdgeByBandpass(in, s1, s1, s1 + 1, s1 + 1);
  } else if (m == 4 || m == 5) {
    // RANK / BILAT need 8 or 32 bpp without a colormap.
    Pix* base = in;
    bool owned = false;
    if (pixGetColormap(in) || (pixGetDepth(in) != 8 && pixGetDepth(in) != 32)) {
      base = pixConvertTo32(in);
      owned = true;
      if (!base) return nullptr;
    }
    if (m == 4) {
      int wf = 2 * std::max(1, r) + 1;
      out = pixRankFilter(base, wf, wf, rk);
    } else {
      float range = 5.f + amt * 75.f;  // AMOUNT as the tonal range: 5..80 gray levels
      out = pixBilateral(base, std::max(1.f, (float)r), range, 10, 1);
    }
    if (owned) pixDestroy(&base);
  } else {
    out = (r <= 0) ? pixCopy(nullptr, in) : pixBlockconv(in, r, r);
  }
  return out;  // native depth (edges mode emits 8 bpp gray) - preview + surface upconvert
}
void Convolve::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("mode");
  writer.Int(mode);
  writer.Key("radius");
  writer.Int(radius);
  writer.Key("amount");
  writer.Double(amount);
  writer.Key("rank");
  writer.Double(rank);
}
bool Convolve::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "mode") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) mode = v;
  } else if (key == "radius") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) radius = v;
  } else if (key == "amount") {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) amount = std::clamp((float)x, 0.f, 1.f);
  } else if (key == "rank") {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) rank = std::clamp((float)x, 0.f, 1.f);
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct ConvolveToy;
struct ConvRadiusDrag : Action {
  TrackedPtr<ConvolveToy> widget;
  ConvRadiusDrag(ui::Pointer& p, ConvolveToy& w);
  ~ConvRadiusDrag();
  void Update() override;
};
struct ConvPoke : Action {
  ConvPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct ConvAmountDrag : Action {
  TrackedPtr<ConvolveToy> widget;
  ConvAmountDrag(ui::Pointer& p, ConvolveToy& w);
  ~ConvAmountDrag();
  void Update() override;
};

struct ConvRankDrag : Action {
  TrackedPtr<ConvolveToy> widget;
  ConvRankDrag(ui::Pointer& p, ConvolveToy& w);
  ~ConvRankDrag();
  void Update() override;
};

struct ConvolveToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int mode = 0;
  int radius = 3;
  bool radius_driven = false;
  float amount = 0.5f;
  float rank = 0.5f;
  bool dragging = false;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kFaceTop = 1.3_cm;
  constexpr static float kFaceBottom = -3.7_cm;
  constexpr static float kLensCY = 1.55_cm;
  constexpr static float kLensR = 1.5_cm;
  constexpr static int kMaxR = 15;

  Ptr<Convolve> LockConv() const { return LockObject<Convolve>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  ConvolveToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockConv()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      mode = t->mode;
      radius = t->radius;
    }
  }

  bool CenteredAtZero() const override { return true; }

  float WheelCX() const { return -1.6_cm; }
  float WheelCY() const { return -0.6_cm; }
  float WheelR() const { return 0.5_cm; }
  Rect RadiusSliderM() const { return Rect(-0.55_cm, -0.8_cm, 1.9_cm, -0.4_cm); }
  Rect AmountSliderM() const { return Rect(-0.55_cm, -1.55_cm, 1.9_cm, -1.15_cm); }
  Rect RankSliderM() const { return Rect(-0.55_cm, -2.3_cm, 1.9_cm, -1.9_cm); }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRRect(RRect::MakeSimple(Rect(-kHalfW, kFaceBottom, kHalfW, kFaceTop), 4_mm).sk);
    b.addOval(SkRect::MakeXYWH(-kLensR, kLensCY - kLensR, 2 * kLensR, 2 * kLensR));
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::RRect(RRect::MakeSimple(Rect(-kHalfW, kFaceBottom, kHalfW, kFaceTop), 4_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kFaceBottom - 1.0_cm, kHalfW + 0.5_cm, kLensCY + kLensR + 0.5_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, kLensCY}, .dir = 180_deg};
    if (&arg == static_cast<const Interface::Table*>(&Convolve::radius_src_tbl))
      return {.pos = {-kHalfW, kLensCY - 1.0_cm}, .dir = 180_deg};  // input, below Paper
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t ConvHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockConv()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)t->mode;
      h *= 16777619u;
      h ^= (uint32_t)t->radius;
      h *= 16777619u;
      h ^= (uint32_t)std::lround(t->amount * 256.f);
      h *= 16777619u;
      h ^= (uint32_t)std::lround(t->rank * 256.f);
      h *= 16777619u;
      float dr;
      if (t->DrivenRadius(dr)) {
        h ^= (uint32_t)(int)std::lround(dr * 4.f) * 2246822519u;
        h ^= 0x0D11A0EDu;
      }
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockConv()) {
      uint32_t h = ConvHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        mode = t->mode;
        radius = t->radius;
        amount = t->amount;
        rank = t->rank;
      }
      float dr;
      if (t->DrivenRadius(dr)) {
        radius_driven = true;
        radius = (int)std::lround(dr);
      } else {
        radius_driven = false;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    SkPath lens = SkPath::Circle(0, kLensCY, kLensR);
    if (cached_preview) {
      canvas.save();
      canvas.clipPath(lens, true);
      float iw = cached_preview->width(), ih = cached_preview->height();
      float s = std::max(2 * kLensR / iw, 2 * kLensR / ih);
      Rect fit = Rect::MakeCenter({0, kLensCY}, iw * s, ih * s);
      SkRect src = SkRect::Make(cached_preview->dimensions());
      SkMatrix m = SkMatrix::RectToRect(src, fit.sk, SkMatrix::kFill_ScaleToFit);
      m.preTranslate(0, cached_preview->height() / 2.f);
      m.preScale(1, -1);
      m.preTranslate(0, -cached_preview->height() / 2.f);
      canvas.concat(m);
      canvas.drawImage(cached_preview, 0, 0, SkSamplingOptions(), nullptr);
      canvas.restore();
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#cabfae"_color);
      canvas.drawPath(lens, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto PX = [&](float m) { return m / kPxToMetric; };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xD01, 1);
      {
        SkPoint lr = P(-kLensR, kLensCY - 0.55_cm);
        ui::leptonica::DrawDepthChip(canvas,
                                     SkRect::MakeXYWH(lr.fX + 4.f - 54.f, lr.fY, 54.f, 24.f),
                                     out_depth, out_cmap, slop::State::Default, 0xD09);
      }
      slop::SketchyStroke(
          canvas,
          slop::WobbleEllipse(P(0, kLensCY), PX(kLensR), PX(kLensR), slop::kWonk, 0xD10, 56),
          slop::kInk, slop::kStrokeBold, 0xD11, 2);

      const char* modes[] = {"BLUR", "SHARP", "EDGE", "BAND", "RANK", "BILAT"};
      ui::leptonica::DrawModeWheel(canvas, P(WheelCX(), WheelCY()), PX(WheelR()), modes, 6, mode,
                                   slop::State::Default, 0xD20);

      Rect rs = RadiusSliderM();
      SkRect rspx = SkRect::MakeLTRB(PX(rs.left), -rs.top / kPxToMetric, PX(rs.right),
                                     -rs.bottom / kPxToMetric);
      slop::Slider(canvas, rspx, std::clamp((float)radius / kMaxR, 0.f, 1.f),
                   radius_driven ? slop::State::Disabled : slop::State::Default, 0xD30);
      {
        char buf[28];
        if (radius_driven)
          snprintf(buf, sizeof(buf), "RADIUS %d DRIVEN", radius);
        else
          snprintf(buf, sizeof(buf), "RADIUS %d", radius);
        slop::DrawText(canvas, buf, P(rs.left, rs.top + 0.12_cm), 15.f, kLabelInk, false, 0);
      }
      // AMOUNT: SHARP's strength; reused as BILAT's tonal RANGE. Greyed elsewhere (a strength
      // could apply but the mode doesn't take one).
      {
        Rect as = AmountSliderM();
        SkRect aspx = SkRect::MakeLTRB(as.left / kPxToMetric, -as.top / kPxToMetric,
                                       as.right / kPxToMetric, -as.bottom / kPxToMetric);
        bool active = mode == 1 || mode == 5;
        slop::Slider(canvas, aspx, std::clamp(amount, 0.f, 1.f),
                     active ? slop::State::Default : slop::State::Disabled, 0xD31);
        char buf2[24];
        if (mode == 5) {
          snprintf(buf2, sizeof(buf2), "RANGE %d", (int)std::lround(5.f + amount * 75.f));
        } else {
          snprintf(buf2, sizeof(buf2), "AMOUNT %.2f", amount);
        }
        slop::DrawText(canvas, buf2, P(as.left, as.top + 0.12_cm), 14.f, kLabelInk, false, 0);
      }
      // RANK: the selection axis (which sorted neighbor wins). It does not exist outside RANK
      // mode, so the whole row hides rather than greys.
      if (mode == 4) {
        Rect ks = RankSliderM();
        SkRect kspx = SkRect::MakeLTRB(ks.left / kPxToMetric, -ks.top / kPxToMetric,
                                       ks.right / kPxToMetric, -ks.bottom / kPxToMetric);
        slop::Slider(canvas, kspx, std::clamp(rank, 0.f, 1.f), slop::State::Default, 0xD32);
        char buf3[24];
        snprintf(buf3, sizeof(buf3), "RANK %.2f", rank);
        slop::DrawText(canvas, buf3, P(ks.left, ks.top + 0.12_cm), 14.f, kLabelInk, false, 0);
        float lmy = (ks.top + ks.bottom) * 0.5f - 0.06_cm;
        slop::DrawText(canvas, "MIN", P(ks.left + 0.08_cm, lmy), 10.f, "#8a7d66"_color, false, 0);
        float medw = slop::TextWidth("MED", 10.f) * kPxToMetric;
        slop::DrawText(canvas, "MED", P((ks.left + ks.right - medw) * 0.5f, lmy), 10.f,
                       "#8a7d66"_color, false, 0);
        float maxw = slop::TextWidth("MAX", 10.f) * kPxToMetric;
        slop::DrawText(canvas, "MAX", P(ks.right - maxw - 0.08_cm, lmy), 10.f, "#8a7d66"_color,
                       false, 0);
      }

      std::string title = "CONVOLVE";
      float tcx = -1.05_cm;
      float tpx = slop::TextWidth(title, kTitleTextPx);
      slop::DrawText(canvas, title, P(tcx - tpx * kPxToMetric * 0.5f, -2.62_cm), kTitleTextPx,
                     slop::kInk, true, 0xD40);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fpx = slop::TextWidth(credit, kCreditTextPx);
        slop::DrawText(canvas, credit, P(tcx - fpx * kPxToMetric * 0.5f, -2.89_cm), kCreditTextPx,
                       "#8a7d66"_color, false, 0xD41);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint pp = {pos.x / kPxToMetric, -pos.y / kPxToMetric};
      int m = ui::leptonica::ModeWheelHit({WheelCX() / kPxToMetric, -WheelCY() / kPxToMetric},
                                          WheelR() / kPxToMetric, pp, 6);
      if (m >= 0) {
        if (auto t = LockConv()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->mode = m;
          }
          t->WakeToys();
        }
        return std::make_unique<ConvPoke>(p);
      }
      Rect rs = RadiusSliderM();
      if (!radius_driven && pos.x >= rs.left - 0.2_cm && pos.x <= rs.right + 0.2_cm &&
          pos.y >= rs.bottom - 0.2_cm && pos.y <= rs.top + 0.2_cm)
        return std::make_unique<ConvRadiusDrag>(p, *this);
      Rect as2 = AmountSliderM();
      if ((mode == 1 || mode == 5) && pos.x >= as2.left - 0.2_cm && pos.x <= as2.right + 0.2_cm &&
          pos.y >= as2.bottom - 0.2_cm && pos.y <= as2.top + 0.2_cm)
        return std::make_unique<ConvAmountDrag>(p, *this);
      Rect ks = RankSliderM();
      if (mode == 4 && pos.x >= ks.left - 0.2_cm && pos.x <= ks.right + 0.2_cm &&
          pos.y >= ks.bottom - 0.2_cm && pos.y <= ks.top + 0.2_cm)
        return std::make_unique<ConvRankDrag>(p, *this);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

ConvRadiusDrag::ConvRadiusDrag(ui::Pointer& p, ConvolveToy& w) : Action(p), widget(&w) {
  if (widget) widget->dragging = true;
}
ConvRadiusDrag::~ConvRadiusDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void ConvRadiusDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect rs = widget->RadiusSliderM();
  float t = std::clamp((pos.x - rs.left) / std::max(1e-4f, rs.Width()), 0.f, 1.f);
  int r = (int)std::lround(t * ConvolveToy::kMaxR);
  if (auto c = widget->LockConv()) {
    {
      auto lock = std::lock_guard(c->mutex);
      c->radius = r;
    }
    c->WakeToys();
  }
  widget->WakeAnimation();
}

ConvAmountDrag::ConvAmountDrag(ui::Pointer& p, ConvolveToy& w) : Action(p), widget(&w) {
  if (widget) widget->dragging = true;
}
ConvAmountDrag::~ConvAmountDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void ConvAmountDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = widget->AmountSliderM();
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  if (auto c = widget->LockObject<Convolve>()) {
    {
      auto lock = std::lock_guard(c->mutex);
      c->amount = t;
    }
    c->WakeToys();
  }
  widget->WakeAnimation();
}

ConvRankDrag::ConvRankDrag(ui::Pointer& p, ConvolveToy& w) : Action(p), widget(&w) {
  if (widget) widget->dragging = true;
}
ConvRankDrag::~ConvRankDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void ConvRankDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = widget->RankSliderM();
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  if (auto c = widget->LockObject<Convolve>()) {
    {
      auto lock = std::lock_guard(c->mutex);
      c->rank = t;
    }
    c->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Convolve::MakeToy(ui::Widget* parent) {
  return std::make_unique<ConvolveToy>(parent, *this);
}

// ============================================================================
// Blend
// ============================================================================
// Blend two same-size 32-bpp pix per mode, scaled by `amount`.
static Pix* BlendApply(Pix* a, Pix* b, int mode, float amount) {
  int w = pixGetWidth(a), h = pixGetHeight(a);
  Pix* out = pixCreate(w, h, 32);
  if (!out) return nullptr;
  l_uint32 *da = pixGetData(a), *db = pixGetData(b), *dd = pixGetData(out);
  int wa = pixGetWpl(a), wb = pixGetWpl(b), wd = pixGetWpl(out);
  amount = std::clamp(amount, 0.f, 1.f);
  auto chan = [&](int av, int bv) {
    int r;
    switch (mode) {
      case 1:
        r = std::min(255, av + bv);
        break;  // add
      case 2:
        r = av * bv / 255;
        break;  // multiply
      case 3:
        r = std::abs(av - bv);
        break;  // difference
      default:
        r = bv;
        break;  // mix (crossfade)
    }
    return std::clamp((int)std::lround(av + (r - av) * amount), 0, 255);
  };
  for (int y = 0; y < h; ++y) {
    l_uint32 *la = da + (size_t)y * wa, *lb = db + (size_t)y * wb, *lo = dd + (size_t)y * wd;
    for (int x = 0; x < w; ++x) {
      l_uint32 pa = la[x], pb = lb[x];
      int rr = chan((pa >> 24) & 0xff, (pb >> 24) & 0xff);
      int gg = chan((pa >> 16) & 0xff, (pb >> 16) & 0xff);
      int bb = chan((pa >> 8) & 0xff, (pb >> 8) & 0xff);
      lo[x] = ((l_uint32)rr << 24) | ((l_uint32)gg << 16) | ((l_uint32)bb << 8) | (pa & 0xff);
    }
  }
  return out;
}

Pix* Blend::ResolveBPix() const {
  auto nested = paperB->FindInterface();
  Object* owner = nested.Owner<Object>();
  if (!owner) return nullptr;
  ImageProvider ip = owner->As<ImageProvider>();
  if (!ip) return nullptr;
  sk_sp<SkImage> img = ip.GetImage();
  if (!img) return nullptr;
  return SkImageToPix(img);
}

Pix* Blend::ApplyOp(Pix* in, const float*) const {
  int m;
  float amt;
  {
    auto lock = std::lock_guard(mutex);
    m = mode;
    amt = amount;
  }
  Pix* b = ResolveBPix();
  if (!b) return pixCopy(nullptr, in);  // no second image yet -> A unchanged
  int w = pixGetWidth(in), h = pixGetHeight(in);
  if (pixGetWidth(b) != w || pixGetHeight(b) != h) {
    Pix* bs = pixScaleToSize(b, w, h);
    pixDestroy(&b);
    b = bs;
    if (!b) return pixCopy(nullptr, in);
  }
  Pix* out = BlendApply(in, b, m, amt);
  pixDestroy(&b);
  return out ? out : pixCopy(nullptr, in);
}
void Blend::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("mode");
  writer.Int(mode);
  writer.Key("amount");
  writer.Double(amount);
}
bool Blend::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "mode") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) mode = v;
  } else if (key == "amount") {
    float v = 0;
    d.Get(v, status);
    if (OK(status)) amount = v;
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct BlendToy;
struct BlendAmountDrag : Action {
  TrackedPtr<BlendToy> widget;
  BlendAmountDrag(ui::Pointer& p, BlendToy& w);
  ~BlendAmountDrag();
  void Update() override;
};
struct BlendPoke : Action {
  BlendPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

// Operator glyphs for the wheel face: MIX, ADD, MULT, DIFF.
static const SkPath* BlendWheelGlyphs() {
  static const std::array<SkPath, 4> paths = [] {
    std::array<SkPath, 4> g;
    {  // MIX: two offset squares, the overlap punched (even-odd) = the sheets interleave
      SkPathBuilder b;
      b.setFillType(SkPathFillType::kEvenOdd);
      b.addRect(SkRect::MakeLTRB(-0.6f, -0.52f, 0.25f, 0.3f));
      b.addRect(SkRect::MakeLTRB(-0.25f, -0.3f, 0.6f, 0.52f));
      g[0] = b.detach();
    }
    {  // ADD: +
      SkPathBuilder b;
      b.addRect(SkRect::MakeLTRB(-0.14f, -0.52f, 0.14f, 0.52f));
      b.addRect(SkRect::MakeLTRB(-0.52f, -0.14f, 0.52f, 0.14f));
      g[1] = b.detach();
    }
    {  // MULT: ×
      SkPathBuilder b;
      SkPath bar = SkPath::Rect(SkRect::MakeLTRB(-0.55f, -0.13f, 0.55f, 0.13f));
      b.addPath(bar.makeTransform(SkMatrix::RotateDeg(45.f)));
      b.addPath(bar.makeTransform(SkMatrix::RotateDeg(-45.f)));
      g[2] = b.detach();
    }
    {  // DIFF: −
      SkPathBuilder b;
      b.addRect(SkRect::MakeLTRB(-0.52f, -0.14f, 0.52f, 0.14f));
      g[3] = b.detach();
    }
    return g;
  }();
  return paths.data();
}

struct BlendToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  uint32_t preview_b_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int mode = 0;
  float amount = 0.5f;
  bool dragging = false;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.3_cm;
  constexpr static float kFaceTop = 2.0_cm;
  constexpr static float kFaceBottom = -2.6_cm;

  Ptr<Blend> LockBlend() const { return LockObject<Blend>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  BlendToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockBlend()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      mode = t->mode;
      amount = t->amount;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewRectM() const { return Rect(0.1_cm, -0.1_cm, kHalfW - 0.3_cm, 1.8_cm); }
  float WheelCX() const { return -1.95_cm; }
  float WheelCY() const { return 0.75_cm; }
  float WheelR() const { return 0.55_cm; }
  Rect AmountSliderM() const { return Rect(-2.75_cm, -0.95_cm, -0.45_cm, -0.55_cm); }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRRect(RRect::MakeSimple(Rect(-kHalfW + 0.45_cm, kFaceBottom + 0.45_cm, kHalfW + 0.45_cm,
                                      kFaceTop + 0.45_cm),
                                 4_mm)
                   .sk);
    b.addRRect(RRect::MakeSimple(Rect(-kHalfW, kFaceBottom, kHalfW, kFaceTop), 4_mm).sk);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::RRect(RRect::MakeSimple(Rect(-kHalfW, kFaceBottom, kHalfW, kFaceTop), 4_mm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kFaceBottom - 1.0_cm, kHalfW + 1.0_cm, kFaceTop + 1.0_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 1.0_cm}, .dir = 180_deg};
    if (&arg == static_cast<const Interface::Table*>(&Blend::paperB_tbl))
      return {.pos = {-kHalfW, -0.2_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t BId() const {
    if (auto t = LockBlend()) {
      auto nested = t->paperB->FindInterface();
      Object* owner = nested.Owner<Object>();
      if (owner) {
        ImageProvider ip = owner->As<ImageProvider>();
        if (ip) {
          sk_sp<SkImage> img = ip.GetImage();
          if (img) return SourceImageId(img);
        }
      }
    }
    return 0;
  }
  uint32_t BlendHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockBlend()) {
      auto lock = std::lock_guard(t->mutex);
      uint32_t am;
      std::memcpy(&am, &t->amount, 4);
      h ^= (uint32_t)t->mode;
      h *= 16777619u;
      h ^= am;
      h *= 16777619u;
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);  // ApplyOp resolves the 2nd image (B) itself
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockBlend()) {
      uint32_t h = BlendHash();
      uint32_t bid = BId();
      if (h != preview_hash || bid != preview_b_id) {
        preview_hash = h;
        preview_b_id = bid;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        mode = t->mode;
        amount = t->amount;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    Rect pr = PreviewRectM();
    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, pr);
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#d8cdbb"_color);
      canvas.drawRect(pr.sk, ph);
      SkPaint key;
      key.setAntiAlias(true);
      key.setStyle(SkPaint::kStroke_Style);
      key.setStrokeWidth(0.3_mm);
      key.setColor("#3a342c"_color);
      canvas.drawRect(pr.sk, key);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xE01, 1);
      DrawDepthChipPx(canvas,
                      SkRect::MakeLTRB(pr.left / kPxToMetric, -pr.top / kPxToMetric,
                                       pr.right / kPxToMetric, -pr.bottom / kPxToMetric),
                      cached_preview, out_depth, out_cmap, 0xE09);

      const char* modes[] = {"MIX", "ADD", "MULT", "DIFF"};
      ui::leptonica::DrawModeWheel(canvas, P(WheelCX(), WheelCY()), WheelR() / kPxToMetric, modes,
                                   4, mode, slop::State::Default, 0xE10, BlendWheelGlyphs());

      Rect as = AmountSliderM();
      SkRect aspx = SkRect::MakeLTRB(as.left / kPxToMetric, -as.top / kPxToMetric,
                                     as.right / kPxToMetric, -as.bottom / kPxToMetric);
      slop::Slider(canvas, aspx, std::clamp(amount, 0.f, 1.f), slop::State::Default, 0xE20);
      {
        char buf[20];
        snprintf(buf, sizeof(buf), "AMOUNT %d%%", (int)std::lround(amount * 100));
        slop::DrawText(canvas, buf, P(as.left, as.top + 0.12_cm), 14.f, kLabelInk, false, 0);
      }
      slop::DrawText(canvas, "A", P(-kHalfW + 0.12_cm, 1.0_cm + 0.12_cm), 16.f, kLabelInk, false,
                     0);
      slop::DrawText(canvas, "B", P(-kHalfW + 0.12_cm, -0.2_cm + 0.12_cm), 16.f, kLabelInk, false,
                     0);

      std::string title = "BLEND";
      float tcx = -0.2_cm;
      float tpx = slop::TextWidth(title, kTitleTextPx);
      slop::DrawText(canvas, title, P(tcx - tpx * kPxToMetric * 0.5f, -1.25_cm), kTitleTextPx,
                     slop::kInk, true, 0xE30);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fpx = slop::TextWidth(credit, kCreditTextPx);
        slop::DrawText(canvas, credit, P(tcx - fpx * kPxToMetric * 0.5f, -1.62_cm), kCreditTextPx,
                       "#8a7d66"_color, false, 0xE31);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint pp = {pos.x / kPxToMetric, -pos.y / kPxToMetric};
      int m = ui::leptonica::ModeWheelHit({WheelCX() / kPxToMetric, -WheelCY() / kPxToMetric},
                                          WheelR() / kPxToMetric, pp, 4);
      if (m >= 0) {
        if (auto t = LockBlend()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->mode = m;
          }
          t->WakeToys();
        }
        return std::make_unique<BlendPoke>(p);
      }
      Rect as = AmountSliderM();
      if (pos.x >= as.left - 0.2_cm && pos.x <= as.right + 0.2_cm && pos.y >= as.bottom - 0.2_cm &&
          pos.y <= as.top + 0.2_cm)
        return std::make_unique<BlendAmountDrag>(p, *this);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

BlendAmountDrag::BlendAmountDrag(ui::Pointer& p, BlendToy& w) : Action(p), widget(&w) {
  if (widget) widget->dragging = true;
}
BlendAmountDrag::~BlendAmountDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void BlendAmountDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect as = widget->AmountSliderM();
  float t = std::clamp((pos.x - as.left) / std::max(1e-4f, as.Width()), 0.f, 1.f);
  if (auto b = widget->LockBlend()) {
    {
      auto lock = std::lock_guard(b->mutex);
      b->amount = t;
    }
    b->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Blend::MakeToy(ui::Widget* parent) {
  return std::make_unique<BlendToy>(parent, *this);
}

// ============================================================================
// Quantize
// ============================================================================
// Returns the colormapped (<=8 bpp) pix directly; callers upconvert for
// display, and the toy reads the palette off the colormap.
bool Quantize::DrivenColors(float& out) const {
  if (auto* src = ncolors_src->ObjectOrNull()) {
    auto txt = src->GetText();
    if (!txt.empty()) {
      char* end = nullptr;
      double v = strtod(txt.c_str(), &end);
      if (end != txt.c_str()) {
        out = std::clamp((float)v, 2.f, 256.f);
        return true;
      }
    }
  }
  return false;
}

Pix* Quantize::ResolveBPix() const {
  auto nested = paperB->FindInterface();
  Object* owner = nested.Owner<Object>();
  if (!owner) return nullptr;
  ImageProvider ip = owner->As<ImageProvider>();
  if (!ip) return nullptr;
  sk_sp<SkImage> img = ip.GetImage();
  if (!img) return nullptr;
  return SkImageToPix(img);
}

// Render a colormap as a swatch strip (8 cells per row, 24 px cells) - the palette made visible.
static Pix* PaletteStripPix(PIXCMAP* cmap) {
  int nc = cmap ? pixcmapGetCount(cmap) : 0;
  if (nc <= 0) return nullptr;
  constexpr int kCell = 24, kPerRow = 8;
  int cols = std::min(nc, kPerRow), rows = (nc + kPerRow - 1) / kPerRow;
  Pix* strip = pixCreate(cols * kCell, rows * kCell, 32);
  if (!strip) return nullptr;
  pixSetAllArbitrary(strip, 0xF4EFE4FF);  // image behind a ragged last row
  for (int i = 0; i < nc; ++i) {
    l_int32 r = 0, g = 0, b = 0;
    pixcmapGetColor(cmap, i, &r, &g, &b);
    l_uint32 word = ((l_uint32)r << 24) | ((l_uint32)g << 16) | ((l_uint32)b << 8) | 0xFF;
    int cx = (i % kPerRow) * kCell, cy = (i / kPerRow) * kCell;
    for (int y = cy; y < cy + kCell; ++y)
      for (int x = cx; x < cx + kCell; ++x) pixSetPixel(strip, x, y, word);
  }
  return strip;
}

Pix* Quantize::ApplyOp(Pix* in, const float*) const {
  int n, ng, a;
  bool d, pal;
  {
    auto lock = std::lock_guard(mutex);
    n = std::clamp(ncolors, 2, 256);
    ng = std::clamp(ngray, 2, 100);
    d = dither;
    a = algo;
    pal = emit_palette;
  }
  float driven_n;
  if (DrivenColors(driven_n)) n = (int)std::lround(driven_n);  // the Colors port wins
  Pix* indexed = nullptr;
  if (a == 3) {
    // FROM B: quantize with the OTHER photo's palette (derive one by octree if B carries none).
    Pix* b = ResolveBPix();
    if (!b) return pixCopy(nullptr, in);  // no palette source yet -> A unchanged
    PIXCMAP* bc = pixGetColormap(b);
    Pix* derived = nullptr;
    if (!bc) {
      // octree wants a big budget (>=128); a flat poster still yields only its real colours
      derived = pixOctreeColorQuant(b, 240, 0);
      bc = derived ? pixGetColormap(derived) : nullptr;
    }
    if (bc) indexed = pixOctcubeQuantFromCmap(in, bc, 8, 4, L_EUCLIDEAN_DISTANCE);
    if (derived) pixDestroy(&derived);
    pixDestroy(&b);
    if (!indexed) return pixCopy(nullptr, in);
  } else if (a == 2) {
    // MIXED: median cut keeping colour and gray populations apart.
    indexed = pixMedianCutQuantMixed(in, n, ng, 5, 250, 15);
  } else if (a == 1) {
    // OCTREE: the count-driven octree (subsample 0 = auto). No dither in this path.
    indexed = pixOctreeQuantNumColors(in, n, 0);
  } else {
    // MEDIAN: outdepth 0 = auto, sigbits/maxsub 0 = defaults, checkbw 1 = gray fallback.
    indexed = pixMedianCutQuantGeneral(in, d ? 1 : 0, 0, n, 0, 0, 1);
  }
  if (!indexed) return nullptr;
  if (pal) {
    Pix* strip = PaletteStripPix(pixGetColormap(indexed));
    pixDestroy(&indexed);
    return strip;  // the exposed product: the palette itself
  }
  return indexed;
}
void Quantize::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("ncolors");
  writer.Int(ncolors);
  writer.Key("ngray");
  writer.Int(ngray);
  writer.Key("dither");
  writer.Bool(dither);
  writer.Key("algo");
  writer.Int(algo);
  writer.Key("emit_palette");
  writer.Bool(emit_palette);
}
bool Quantize::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "ncolors") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) ncolors = std::clamp(v, 2, 256);
  } else if (key == "ngray") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) ngray = std::clamp(v, 2, 100);
  } else if (key == "emit_palette") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) emit_palette = v;
  } else if (key == "dither") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) dither = v;
  } else if (key == "algo") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) algo = std::clamp(v, 0, 3);
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

// Discrete palette sizes the count control snaps to (countable at the low end, doubling up high).
static constexpr int kQuantSteps[] = {2, 4, 8, 16, 32, 64, 128, 256};
static constexpr int kQuantNumSteps = 8;
static int QuantNearestStepIdx(int n) {
  int best = 0, bd = 1 << 30;
  for (int i = 0; i < kQuantNumSteps; ++i) {
    int diff = std::abs(kQuantSteps[i] - n);
    if (diff < bd) {
      bd = diff;
      best = i;
    }
  }
  return best;
}

struct QuantizeToy;
struct QuantGraysDrag : Action {
  TrackedPtr<QuantizeToy> widget;
  QuantGraysDrag(ui::Pointer& p, QuantizeToy& w);
  void Update() override;
};
struct QuantCountDrag : Action {
  TrackedPtr<QuantizeToy> widget;
  QuantCountDrag(ui::Pointer& p, QuantizeToy& w);
  ~QuantCountDrag();
  void Update() override;
};
struct QuantPoke : Action {
  QuantPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct QuantizeToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  std::vector<SkColor> palette;  // the real output colormap (UI thread only)
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int ncolors = 8;
  bool colors_driven = false;
  int ngray = 4;
  bool dither = false;
  int algo = 0;
  bool emit_palette = false;
  int tab_count = 8;  // chips drawn across the top; tracks ncolors (capped)
  bool dragging = false;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kTabTop = 2.2_cm;    // silhouette top edge (chip tips)
  constexpr static float kBodyTop = 1.45_cm;  // where the chips meet the card body
  constexpr static float kBottom = -3.90_cm;
  constexpr static float kTabInset = 0.18_cm;
  constexpr static float kTabGap = 0.06_cm;
  constexpr static int kMaxTabs = 12;

  Ptr<Quantize> LockQuant() const { return LockObject<Quantize>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  QuantizeToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockQuant()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      ncolors = t->ncolors;
      dither = t->dither;
    }
    tab_count = std::clamp(ncolors, 2, kMaxTabs);
  }

  bool CenteredAtZero() const override { return true; }

  Rect CountSliderM() const { return Rect(-2.7_cm, -1.4_cm, 0.8_cm, -1.05_cm); }
  Rect GraysSliderM() const { return Rect(-2.7_cm, -1.85_cm, 0.8_cm, -1.55_cm); }
  SkPoint AlgoWheelCM() const { return {1.05_cm, -2.0_cm}; }
  constexpr static float kAlgoWheelR = 0.55_cm;
  Rect DitherToggleM() const { return Rect(-2.7_cm, -2.35_cm, -1.5_cm, -1.95_cm); }
  Rect PaletteChipM() const { return Rect(-2.7_cm, -3.08_cm, -1.55_cm, -2.72_cm); }
  Rect PreviewM() const { return Rect(-2.85_cm, -0.55_cm, 2.85_cm, 1.2_cm); }

  Rect TabRect(int i, int T) const {
    float x0 = -kHalfW + kTabInset, x1 = kHalfW - kTabInset;
    float tw = (x1 - x0 - (T - 1) * kTabGap) / T;
    float l = x0 + i * (tw + kTabGap);
    return Rect(l, kBodyTop - 0.02_cm, l + tw, kTabTop);
  }

  SkPath Shape() const override {
    int T = std::clamp(tab_count, 2, kMaxTabs);
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    for (int i = 0; i < T; ++i) b.addRect(TabRect(i, T).sk);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kTabTop).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, kTabTop + 0.3_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.3_cm}, .dir = 180_deg};
    if (&arg == static_cast<const Interface::Table*>(&Quantize::paperB_tbl))
      return {.pos = {-kHalfW, -0.3_cm}, .dir = 180_deg};
    if (&arg == static_cast<const Interface::Table*>(&Quantize::ncolors_src_tbl))
      return {.pos = {-kHalfW, -0.9_cm}, .dir = 180_deg};  // input, below Palette
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t QuantHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockQuant()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)t->ncolors;
      h *= 16777619u;
      h ^= (uint32_t)t->ngray;
      h *= 16777619u;
      h ^= (uint32_t)(t->dither ? 1 : 0);
      h *= 16777619u;
      h ^= (uint32_t)t->algo;
      h *= 16777619u;
      h ^= (uint32_t)(t->emit_palette ? 1 : 0);
      h *= 16777619u;
      float dn;
      if (t->DrivenColors(dn)) {
        h ^= (uint32_t)(int)std::lround(dn) * 2654435761u;
        h ^= 0xC0105EEDu;
      }
      if (t->algo == 3) {
        auto nested = t->paperB->FindInterface();
        Object* owner = nested.Owner<Object>();
        ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
        h ^= SourceImageId(ip ? ip.GetImage() : nullptr);
        h *= 16777619u;
      }
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    palette.clear();
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    // Read the real output palette off the colormap before upconverting for display.
    if (PIXCMAP* cmap = pixGetColormap(result)) {
      int nc = pixcmapGetCount(cmap);
      for (int i = 0; i < nc && i < 256; ++i) {
        l_int32 r = 0, g = 0, bch = 0;
        pixcmapGetColor(cmap, i, &r, &g, &bch);
        palette.push_back(SkColorSetRGB(r, g, bch));
      }
    }
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockQuant()) {
      uint32_t h = QuantHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        ncolors = t->ncolors;
        ngray = t->ngray;
        dither = t->dither;
        algo = t->algo;
        emit_palette = t->emit_palette;
      }
      float dn;
      if (t->DrivenColors(dn)) {
        colors_driven = true;
        ncolors = (int)std::lround(dn);
      } else {
        colors_driven = false;
      }
      tab_count = std::clamp(ncolors, 2, kMaxTabs);
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    int T = std::clamp(tab_count, 2, kMaxTabs);
    for (int i = 0; i < T; ++i) {
      SkPaint chip;
      chip.setAntiAlias(true);
      if (!palette.empty()) {
        int idx = (int)((float)i * palette.size() / T);
        chip.setColor(palette[std::clamp(idx, 0, (int)palette.size() - 1)]);
      } else {
        chip.setColor("#cabfae"_color);
      }
      canvas.drawRect(TabRect(i, T).sk, chip);
    }

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto PX = [&](float m) { return m / kPxToMetric; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(PX(r.left), -r.top / kPxToMetric, PX(r.right),
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xE01, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0xEA0);

      Rect cs = CountSliderM();
      float ct = (float)QuantNearestStepIdx(ncolors) / (kQuantNumSteps - 1);
      slop::Slider(canvas, RPX(cs), std::clamp(ct, 0.f, 1.f),
                   colors_driven ? slop::State::Disabled : slop::State::Default, 0xE10);
      {
        char buf[28];
        if (colors_driven)
          snprintf(buf, sizeof(buf), "COLORS %d DRIVEN", ncolors);
        else
          snprintf(buf, sizeof(buf), "COLORS %d", ncolors);
        slop::DrawText(canvas, buf, P(cs.left, cs.top + 0.14_cm), 15.f, kLabelInk, false, 0);
      }

      if (algo == 2) {
        Rect gs = GraysSliderM();
        slop::Slider(canvas, RPX(gs), std::clamp((ngray - 2) / 98.f, 0.f, 1.f),
                     slop::State::Default, 0xE15);
        char gbuf[20];
        snprintf(gbuf, sizeof(gbuf), "GRAYS %d", ngray);
        slop::DrawText(canvas, gbuf, P(gs.left, gs.top + 0.12_cm), 14.f, kLabelInk, false, 0);
      }

      slop::Toggle(canvas, RPX(DitherToggleM()), dither,
                   algo == 0 ? slop::State::Default : slop::State::Disabled, 0xE20);
      slop::DrawText(canvas, "DITHER", P(DitherToggleM().left, DitherToggleM().top + 0.16_cm), 15.f,
                     kLabelInk, false, 0);

      {
        static const char* const kAlgoLabels[] = {"MEDIAN", "OCTREE", "MIXED", "FROM B"};
        SkPoint wc = P(AlgoWheelCM().fX, AlgoWheelCM().fY);
        ui::leptonica::DrawModeWheel(canvas, wc, kAlgoWheelR / kPxToMetric, kAlgoLabels, 4, algo,
                                     slop::State::Default, 0xE60);
        slop::DrawText(canvas, "ALGORITHM",
                       {wc.fX - slop::TextWidth("ALGORITHM", 12.f) / 2,
                        wc.fY - kAlgoWheelR / kPxToMetric - 8.f},
                       12.f, kLabelInk, false, 0);
      }

      {
        Rect pc = PaletteChipM();
        SkRect ppx2 = RPX(pc);
        SkPath chip = slop::WonkyRoundRect(ppx2, ppx2.height() * 0.3f, slop::kWonk * 0.5f, 0xE70);
        slop::MisregFill(canvas, chip, emit_palette ? slop::kCyan : slop::kPaper, 0xE71);
        slop::SketchyStroke(canvas, chip, slop::kInk,
                            emit_palette ? slop::kStrokeBold : slop::kStroke, 0xE72,
                            emit_palette ? 2 : 1);
        const char* pl = "PALETTE";
        float pw = slop::TextWidth(pl, 12.f);
        slop::DrawText(canvas, pl, {ppx2.centerX() - pw * 0.5f, ppx2.centerY() + 4.f}, 12.f,
                       slop::kInk, false, 0);
      }

      std::string title = "QUANTIZE";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -2.7_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xE30);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -3.02_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0xE31);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      {
        SkPoint wc = AlgoWheelCM();
        SkPoint ppx{pos.x / kPxToMetric, -pos.y / kPxToMetric};
        SkPoint wcpx{wc.fX / kPxToMetric, -wc.fY / kPxToMetric};
        int hit = ui::leptonica::ModeWheelHit(wcpx, kAlgoWheelR / kPxToMetric, ppx, 4);
        if (hit >= 0) {
          if (auto t = LockQuant()) {
            {
              auto lock = std::lock_guard(t->mutex);
              t->algo = hit;
            }
            t->WakeToys();
          }
          return std::make_unique<QuantPoke>(p);
        }
      }
      {
        Rect pc = PaletteChipM();
        if (pos.x >= pc.left && pos.x <= pc.right && pos.y >= pc.bottom - 0.1_cm &&
            pos.y <= pc.top + 0.1_cm) {
          if (auto t = LockQuant()) {
            {
              auto lock = std::lock_guard(t->mutex);
              t->emit_palette = !t->emit_palette;
            }
            t->WakeToys();
          }
          return std::make_unique<QuantPoke>(p);
        }
      }
      if (algo == 2) {
        Rect gs = GraysSliderM();
        if (pos.x >= gs.left - 0.2_cm && pos.x <= gs.right + 0.2_cm &&
            pos.y >= gs.bottom - 0.15_cm && pos.y <= gs.top + 0.15_cm)
          return std::make_unique<QuantGraysDrag>(p, *this);
      }
      Rect dt = DitherToggleM();
      if (algo == 0 && pos.x >= dt.left - 0.15_cm && pos.x <= dt.right + 0.15_cm &&
          pos.y >= dt.bottom - 0.15_cm && pos.y <= dt.top + 0.15_cm) {
        if (auto t = LockQuant()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->dither = !t->dither;
          }
          t->WakeToys();
        }
        return std::make_unique<QuantPoke>(p);
      }
      Rect cs = CountSliderM();
      if (!colors_driven && pos.x >= cs.left - 0.2_cm && pos.x <= cs.right + 0.2_cm &&
          pos.y >= cs.bottom - 0.2_cm && pos.y <= cs.top + 0.2_cm)
        return std::make_unique<QuantCountDrag>(p, *this);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

QuantCountDrag::QuantCountDrag(ui::Pointer& p, QuantizeToy& w) : Action(p), widget(&w) {
  if (widget) widget->dragging = true;
}
QuantCountDrag::~QuantCountDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void QuantCountDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect cs = widget->CountSliderM();
  float t = std::clamp((pos.x - cs.left) / std::max(1e-4f, cs.Width()), 0.f, 1.f);
  int idx = std::clamp((int)std::lround(t * (kQuantNumSteps - 1)), 0, kQuantNumSteps - 1);
  int n = kQuantSteps[idx];
  if (auto q = widget->LockQuant()) {
    {
      auto lock = std::lock_guard(q->mutex);
      q->ncolors = n;
    }
    q->WakeToys();
  }
  widget->WakeAnimation();
}

QuantGraysDrag::QuantGraysDrag(ui::Pointer& p, QuantizeToy& w) : Action(p), widget(&w) {}
void QuantGraysDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = widget->GraysSliderM();
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  if (auto q = widget->LockQuant()) {
    {
      auto lock = std::lock_guard(q->mutex);
      q->ngray = 2 + (int)std::lround(t * 98.f);
    }
    q->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Quantize::MakeToy(ui::Widget* parent) {
  return std::make_unique<QuantizeToy>(parent, *this);
}

// ============================================================================
// Flatten
// ============================================================================
// Same depth out as in. thresh/smooth use Leptonica's recommended defaults;
// mincount scales with the tile so small tiles don't trip the mincount check.
Pix* Flatten::ApplyOp(Pix* in, const float*) const {
  int bg, sc, m;
  bool map;
  {
    auto lock = std::lock_guard(mutex);
    bg = std::clamp(bgval, 128, 240);
    sc = tilesize;
    m = method;
    map = show_map;
  }
  if (m == 3) {
    // CONTRAST: stretch each tile's range to full scale (local contrast, not illumination).
    int tile = std::clamp(sc, 6, 40);
    Pix* g = pixConvertTo8(in, 0);
    if (!g) return nullptr;
    Pix* out = pixContrastNorm(nullptr, g, tile, tile, 40, 2, 2);
    pixDestroy(&g);
    return out;
  }
  if (m == 2) {
    // FLEX: adaptive flatten for small/varying structure; gray only, no brightness target.
    int n = std::clamp(sc, 3, 10);
    Pix* g = pixConvertTo8(in, 0);
    if (!g) return nullptr;
    Pix* out = pixBackgroundNormFlex(g, n, n, 2, 2, 0);
    pixDestroy(&g);
    return out;
  }
  if (m == 1) {
    // MORPH: estimate the background by grayscale closing (robust over dense ink).
    int size = std::clamp(sc, 3, 21) | 1;
    if (map) {
      Pix* g = pixConvertTo8(in, 0);
      if (!g) return nullptr;
      Pix* mp = nullptr;
      pixGetBackgroundGrayMapMorph(g, nullptr, 4, size, &mp);
      pixDestroy(&g);
      if (!mp) return nullptr;
      Pix* up = pixScaleBySampling(mp, 4.f, 4.f);  // back to image scale; blocky on purpose
      pixDestroy(&mp);
      return up;
    }
    return pixBackgroundNormMorph(in, nullptr, 4, size, bg);
  }
  // TILE: estimate the background per tile.
  int tile = std::clamp(sc, 6, 40);
  int mincount = std::max(1, tile * tile / 3);
  if (map) {
    // the exposed internal product: measure the map, repair its holes, blow it back up.
    Pix* g = pixConvertTo8(in, 0);
    if (!g) return nullptr;
    Pix* mp = nullptr;
    pixGetBackgroundGrayMap(g, nullptr, tile, tile, 60, mincount, &mp);
    pixDestroy(&g);
    if (!mp) return nullptr;
    pixFillMapHoles(mp, pixGetWidth(mp), pixGetHeight(mp), L_FILL_BLACK);
    Pix* up = pixScaleBySampling(mp, (float)tile, (float)tile);
    pixDestroy(&mp);
    return up;
  }
  return pixBackgroundNorm(in, nullptr, nullptr, tile, tile, 60, mincount, bg, 2, 1);
}
void Flatten::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("bgval");
  writer.Int(bgval);
  writer.Key("tilesize");
  writer.Int(tilesize);
  writer.Key("method");
  writer.Int(method);
  writer.Key("show_map");
  writer.Bool(show_map);
}
bool Flatten::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "bgval") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) bgval = std::clamp(v, 128, 240);
  } else if (key == "tilesize") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) tilesize = std::clamp(v, 3, 40);
  } else if (key == "method") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) method = std::clamp(v, 0, 3);
  } else if (key == "show_map") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) show_map = v;
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct FlattenToy;
struct FlattenPoke : Action {
  FlattenPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};
struct FlattenSliderDrag : Action {
  TrackedPtr<FlattenToy> widget;
  int which;  // 0 = bg value, 1 = tile size
  FlattenSliderDrag(ui::Pointer& p, FlattenToy& w, int which);
  ~FlattenSliderDrag();
  void Update() override;
};

struct FlattenToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int bgval = 200;
  int tilesize = 10;
  int method = 0;
  bool show_map = false;
  bool dragging = false;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 1.0_cm;
  constexpr static float kBottom = -5.30_cm;
  constexpr static float kShadeTop = 2.45_cm;
  constexpr static float kShadeBottom = 1.75_cm;
  constexpr static float kShadeTopHalf = 0.5_cm;
  constexpr static float kShadeBotHalf = 1.3_cm;
  constexpr static float kBulbCY = 1.6_cm;
  constexpr static float kBulbR = 0.22_cm;

  Ptr<Flatten> LockFlat() const { return LockObject<Flatten>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  FlattenToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockFlat()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      bgval = t->bgval;
      tilesize = t->tilesize;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect BgSliderM() const { return Rect(-2.7_cm, -1.75_cm, 0.7_cm, -1.4_cm); }
  Rect TileSliderM() const { return Rect(-2.7_cm, -2.55_cm, 0.7_cm, -2.2_cm); }
  Rect PreviewM() const { return Rect(-1.75_cm, -1.0_cm, 1.75_cm, 1.0_cm); }
  SkPoint WheelCM() const { return {1.85_cm, -3.0_cm}; }
  float WheelRM() const { return 0.45_cm; }
  Rect MapChipM() const { return Rect(-2.7_cm, -3.3_cm, -1.55_cm, -2.95_cm); }
  // scale slider range per method: TILE px / MORPH close-size / FLEX tiles
  void ScaleRange(int& lo, int& hi) const {
    if (method == 1) {
      lo = 3;
      hi = 21;
    } else if (method == 2) {
      lo = 3;
      hi = 10;
    } else {
      lo = 6;
      hi = 40;
    }  // TILE and CONTRAST: tile px
  }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    SkPathBuilder shade;
    shade.moveTo(-kShadeTopHalf, kShadeTop);
    shade.lineTo(kShadeTopHalf, kShadeTop);
    shade.lineTo(kShadeBotHalf, kShadeBottom);
    shade.lineTo(-kShadeBotHalf, kShadeBottom);
    shade.close();
    b.addPath(shade.detach());
    // The cord joins the shade to the body; without it the lamp is a separate
    // contour and offset outlines break.
    b.addRect(Rect(-0.13_cm, kBodyTop - 0.05_cm, 0.13_cm, kShadeTop + 0.35_cm).sk);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kShadeTop).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, kShadeTop + 0.6_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, -0.1_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t FlattenHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockFlat()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)t->bgval;
      h *= 16777619u;
      h ^= (uint32_t)t->tilesize;
      h *= 16777619u;
      h ^= (uint32_t)t->method;
      h *= 16777619u;
      h ^= (uint32_t)t->show_map;
      h *= 16777619u;
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockFlat()) {
      uint32_t h = FlattenHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        bgval = t->bgval;
        tilesize = t->tilesize;
        method = t->method;
        show_map = t->show_map;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    SkPaint bulb;
    bulb.setAntiAlias(true);
    bulb.setColor("#f5d469"_color);
    canvas.drawCircle(0, kBulbCY, kBulbR, bulb);

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto PX = [&](float m) { return m / kPxToMetric; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(PX(r.left), -r.top / kPxToMetric, PX(r.right),
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xF01, 1);
      DrawDepthChipPx(
          canvas,
          SkRect::MakeLTRB(PreviewM().left / kPxToMetric, -PreviewM().top / kPxToMetric,
                           PreviewM().right / kPxToMetric, -PreviewM().bottom / kPxToMetric),
          cached_preview, out_depth, out_cmap, 0xF09);

      slop::SketchyStroke(
          canvas,
          slop::WobbleEllipse(P(0, kBulbCY), PX(kBulbR), PX(kBulbR), slop::kWonk, 0xF05, 40),
          slop::kInk, slop::kStroke, 0xF06, 2);
      SkPoint bulbp = P(0, kBulbCY - kBulbR);
      const float ray_dx[] = {-2.0_cm, -1.0_cm, 0.0_cm, 1.0_cm, 2.0_cm};
      for (int i = 0; i < 5; ++i) {
        SkPoint end = P(ray_dx[i], 0.15_cm);
        slop::SketchyStroke(
            canvas, slop::WobbleLine(bulbp, end, slop::kWonk * 0.6f, slop::kSeg, 0xF10u + i),
            "#e7c24a"_color, slop::kStrokeHair, 0xF20u + i, 1);
      }

      Rect bs = BgSliderM();
      float bt = std::clamp((float)(bgval - 128) / (240 - 128), 0.f, 1.f);
      bool has_bg = method == 0 || method == 1;  // FLEX/CONTRAST have no brightness target
      slop::Slider(canvas, RPX(bs), bt, has_bg ? slop::State::Default : slop::State::Disabled,
                   0xF30);
      {
        char buf[28];
        snprintf(buf, sizeof(buf), "BG VALUE %d", bgval);
        slop::DrawText(canvas, buf, P(bs.left, bs.top + 0.14_cm), 15.f, kLabelInk, false, 0);
      }
      Rect ts = TileSliderM();
      int lo, hi;
      ScaleRange(lo, hi);
      int scv = std::clamp(tilesize, lo, hi);
      float tt = std::clamp((float)(scv - lo) / (hi - lo), 0.f, 1.f);
      slop::Slider(canvas, RPX(ts), tt, slop::State::Default, 0xF40);
      {
        char buf[20];
        if (method == 1)
          snprintf(buf, sizeof(buf), "CLOSE %d", scv | 1);
        else
          snprintf(buf, sizeof(buf), "GRID %d", scv);
        slop::DrawText(canvas, buf, P(ts.left, ts.top + 0.14_cm), 15.f, kLabelInk, false, 0);
      }
      {
        const char* methods[] = {"TILE", "MORPH", "FLEX", "CONTRAST"};
        ui::leptonica::DrawModeWheel(
            canvas, {WheelCM().fX / kPxToMetric, -WheelCM().fY / kPxToMetric},
            WheelRM() / kPxToMetric, methods, 4, method, slop::State::Default, 0xF60);
      }
      {
        Rect mc = MapChipM();
        SkRect mpx = RPX(mc);
        bool na = method >= 2;  // FLEX/CONTRAST expose no illumination map
        bool on = show_map && !na;
        SkPath chip = slop::WonkyRoundRect(mpx, mpx.height() * 0.3f, slop::kWonk * 0.5f, 0xF70);
        if (on)
          slop::MisregFill(canvas, chip, slop::kCyan, 0xF71);
        else
          slop::MisregFill(canvas, chip, na ? slop::kGray : slop::kPaper, 0xF71);
        slop::SketchyStroke(canvas, chip, na ? slop::kGray : slop::kInk,
                            on ? slop::kStrokeBold : slop::kStroke, 0xF72, on ? 2 : 1);
        const char* ml = "MAP";
        float mw = slop::TextWidth(ml, 15.f);
        slop::DrawText(canvas, ml, {mpx.centerX() - mw * 0.5f, mpx.centerY() + 5.f}, 15.f,
                       na ? slop::kGrayDark : slop::kInk, false, 0);
        if (na) slop::HatchRect(canvas, mpx, slop::kInkSoft, mpx.height() * 0.3f, 0xF73);
      }

      std::string title = "FLATTEN";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -4.1_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xF50);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -4.4_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0xF51);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      auto inRect = [&](const Rect& r) {
        return pos.x >= r.left - 0.2_cm && pos.x <= r.right + 0.2_cm &&
               pos.y >= r.bottom - 0.2_cm && pos.y <= r.top + 0.2_cm;
      };
      SkPoint pp = {pos.x / kPxToMetric, -pos.y / kPxToMetric};
      int mhit =
          ui::leptonica::ModeWheelHit({WheelCM().fX / kPxToMetric, -WheelCM().fY / kPxToMetric},
                                      WheelRM() / kPxToMetric, pp, 4);
      if (mhit >= 0) {
        if (auto t = LockFlat()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->method = mhit;
          }
          t->WakeToys();
        }
        return std::make_unique<FlattenPoke>(p);
      }
      if (method < 2 && inRect(MapChipM())) {
        if (auto t = LockFlat()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->show_map = !t->show_map;
          }
          t->WakeToys();
        }
        return std::make_unique<FlattenPoke>(p);
      }
      if ((method == 0 || method == 1) && inRect(BgSliderM()))
        return std::make_unique<FlattenSliderDrag>(p, *this, 0);
      if (inRect(TileSliderM())) return std::make_unique<FlattenSliderDrag>(p, *this, 1);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

FlattenSliderDrag::FlattenSliderDrag(ui::Pointer& p, FlattenToy& w, int which)
    : Action(p), widget(&w), which(which) {
  if (widget) widget->dragging = true;
}
FlattenSliderDrag::~FlattenSliderDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void FlattenSliderDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = which == 0 ? widget->BgSliderM() : widget->TileSliderM();
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  if (auto n = widget->LockFlat()) {
    {
      auto lock = std::lock_guard(n->mutex);
      if (which == 0) {
        n->bgval = 128 + (int)std::lround(t * (240 - 128));
      } else {
        int lo, hi;
        widget->ScaleRange(lo, hi);
        n->tilesize = lo + (int)std::lround(t * (hi - lo));
      }
    }
    n->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Flatten::MakeToy(ui::Widget* parent) {
  return std::make_unique<FlattenToy>(parent, *this);
}

// ============================================================================
// Posterize
// ============================================================================
// A 256-entry staircase NUMA applied per channel with pixTRCMap.
Pix* Posterize::ApplyOp(Pix* in, const float*) const {
  int n;
  {
    auto lock = std::lock_guard(mutex);
    n = std::clamp(levels, 2, 8);
  }
  NUMA* na = numaCreate(256);
  if (!na) return nullptr;
  for (int i = 0; i < 256; ++i) {
    int idx = std::min(n - 1, i * n / 256);
    numaAddNumber(na, (l_float32)std::lround(idx * 255.0 / (n - 1)));
  }
  Pix* out = pixCopy(nullptr, in);
  if (out && pixTRCMap(out, nullptr, na) != 0) pixDestroy(&out);
  numaDestroy(&na);
  return out;
}
void Posterize::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("levels");
  writer.Int(levels);
}
bool Posterize::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "levels") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) levels = std::clamp(v, 2, 8);
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct PosterizeToy;
struct PosterStepPoke : Action {
  PosterStepPoke(ui::Pointer& p, PosterizeToy& w, int delta);
  void Update() override {}
};

struct PosterizeToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int levels = 4;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;  // where the treads stand on the body
  constexpr static float kBottom = -4.00_cm;
  constexpr static float kStairLow = 1.45_cm;  // top of the first (black) tread
  constexpr static float kStairHigh = 2.5_cm;  // top of the last (white) tread

  Ptr<Posterize> LockPoster() const { return LockObject<Posterize>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  PosterizeToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockPoster()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      levels = t->levels;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect StepperM() const { return Rect(-2.7_cm, -2.3_cm, -0.85_cm, -1.6_cm); }
  Rect PreviewM() const { return Rect(-2.85_cm, -1.15_cm, 2.85_cm, 0.8_cm); }

  Rect TreadRect(int i, int N) const {
    float w = 2 * kHalfW / N;
    float l = -kHalfW + i * w;
    float top = kStairLow + (kStairHigh - kStairLow) * ((float)i / (N - 1));
    return Rect(l, kBodyTop - 0.02_cm, l + w, top);
  }

  SkPath Shape() const override {
    int N = std::clamp(levels, 2, 8);
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    for (int i = 0; i < N; ++i) b.addRect(TreadRect(i, N).sk);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kStairHigh).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, kStairHigh + 0.4_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.0_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t PosterHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockPoster()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)t->levels;
      h *= 16777619u;
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockPoster()) {
      uint32_t h = PosterHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        levels = t->levels;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    int N = std::clamp(levels, 2, 8);
    for (int i = 0; i < N; ++i) {
      int v = (int)std::lround(i * 255.0 / (N - 1));
      SkPaint tread;
      tread.setAntiAlias(true);
      tread.setColor(SkColorSetRGB(v, v, v));
      Rect r = TreadRect(i, N);
      canvas.drawRect(Rect(r.left + 0.15_cm, r.bottom, r.right - 0.15_cm, r.top - 0.15_cm).sk,
                      tread);
    }

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto PX = [&](float m) { return m / kPxToMetric; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(PX(r.left), -r.top / kPxToMetric, PX(r.right),
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xE61, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0xE90);

      Rect st = StepperM();
      char buf[4];
      snprintf(buf, sizeof(buf), "%d", levels);
      slop::Stepper(canvas, RPX(st), buf, slop::State::Default, 0xE70);
      slop::DrawText(canvas, "LEVELS", P(st.left, st.top + 0.14_cm), 15.f, kLabelInk, false, 0);

      std::string title = "POSTERIZE";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -2.85_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xE80);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -3.13_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0xE81);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      Rect st = StepperM();
      float kw = std::min(st.Height(), st.Width() * 0.3f);  // the Stepper's key slice width
      Rect minus = Rect(st.left - 0.1_cm, st.bottom - 0.1_cm, st.left + kw, st.top + 0.1_cm);
      Rect plus = Rect(st.right - kw, st.bottom - 0.1_cm, st.right + 0.1_cm, st.top + 0.1_cm);
      auto in = [&](const Rect& r) {
        return pos.x >= r.left && pos.x <= r.right && pos.y >= r.bottom && pos.y <= r.top;
      };
      if (in(minus)) return std::make_unique<PosterStepPoke>(p, *this, -1);
      if (in(plus)) return std::make_unique<PosterStepPoke>(p, *this, +1);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

PosterStepPoke::PosterStepPoke(ui::Pointer& p, PosterizeToy& w, int delta) : Action(p) {
  if (auto t = w.LockPoster()) {
    {
      auto lock = std::lock_guard(t->mutex);
      t->levels = std::clamp(t->levels + delta, 2, 8);
    }
    t->WakeToys();
  }
  w.WakeAnimation();
}

std::unique_ptr<ObjectToy> Posterize::MakeToy(ui::Widget* parent) {
  return std::make_unique<PosterizeToy>(parent, *this);
}

// ============================================================================
// Dither
// ============================================================================
// Gray the input, then pixDitherToBinarySpec with the clip pair.
Pix* Dither::ApplyOp(Pix* in, const float*) const {
  int lo, up;
  {
    auto lock = std::lock_guard(mutex);
    lo = std::clamp(clip_black, 0, 64);
    up = std::clamp(clip_white, 0, 64);
  }
  Pix* gray = pixConvertRGBToLuminance(in);
  if (!gray) return nullptr;
  Pix* out = pixDitherToBinarySpec(gray, lo, up);
  pixDestroy(&gray);
  return out;  // 1 bpp; preview + surface upconvert
}
void Dither::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("clip_black");
  writer.Int(clip_black);
  writer.Key("clip_white");
  writer.Int(clip_white);
}
bool Dither::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "clip_black") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) clip_black = std::clamp(v, 0, 64);
  } else if (key == "clip_white") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) clip_white = std::clamp(v, 0, 64);
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct DitherToy;
// Dragging one of the two clip markers along the Window band.
struct DitherClipDrag : Action {
  TrackedPtr<DitherToy> widget;
  int which;  // 0 = black clip (lo marker), 1 = white clip (hi marker)
  DitherClipDrag(ui::Pointer& p, DitherToy& w, int which);
  ~DitherClipDrag();
  void Update() override;
};

struct DitherToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  std::vector<uint32_t> histogram;
  float max_log_count = 1.0f;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int clip_black = 10;
  int clip_white = 10;
  int dragging_marker = -1;  // 0/1 while a DitherClipDrag is live

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 1.0_cm;
  constexpr static float kBottom = -4.00_cm;
  constexpr static int kBumps = 7;

  Ptr<Dither> LockDither() const { return LockObject<Dither>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  DitherToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockDither()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      clip_black = t->clip_black;
      clip_white = t->clip_white;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.85_cm, -0.8_cm, 2.85_cm, 0.85_cm); }
  Rect WindowM() const { return Rect(-2.7_cm, -2.05_cm, 1.5_cm, -1.55_cm); }

  void BumpCircle(int i, float& cx, float& cy, float& r) const {
    float t = (float)i / (kBumps - 1);
    r = 0.58_cm - t * 0.42_cm;
    cx = -2.45_cm + t * 4.9_cm;
    cy = kBodyTop + r * 0.25f;
  }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    for (int i = 0; i < kBumps; ++i) {
      float cx, cy, r;
      BumpCircle(i, cx, cy, r);
      b.addOval(Rect(cx - r, cy - r, cx + r, cy + r).sk);
    }
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop + 0.8_cm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, kBodyTop + 1.2_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.0_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t DitherHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockDither()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)t->clip_black;
      h *= 16777619u;
      h ^= (uint32_t)t->clip_white;
      h *= 16777619u;
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    histogram.assign(256, 0);
    max_log_count = 1.0f;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    // Luminance histogram of the source - the data the clip markers act on.
    {
      int w = pixGetWidth(pix), h = pixGetHeight(pix);
      l_int32 wpl = pixGetWpl(pix);
      l_uint32* d = pixGetData(pix);
      for (int y = 0; y < h; ++y) {
        l_uint32* line = d + (size_t)y * wpl;
        for (int x = 0; x < w; ++x) {
          uint32_t rgba = __builtin_bswap32(line[x]);
          float r = (rgba >> 24) & 0xff, gg = (rgba >> 16) & 0xff, b = (rgba >> 8) & 0xff;
          int lum = (int)(0.3f * r + 0.59f * gg + 0.11f * b);
          histogram[std::clamp(lum, 0, 255)]++;
        }
      }
      for (uint32_t c : histogram)
        if (c > 0) max_log_count = std::max(max_log_count, std::log((float)c + 1.f));
    }
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockDither()) {
      uint32_t h = DitherHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        clip_black = t->clip_black;
        clip_white = t->clip_white;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    for (int i = 0; i < kBumps; ++i) {
      float cx, cy, r;
      BumpCircle(i, cx, cy, r);
      float ir = std::max(r - 0.13_cm, 0.045_cm);
      SkPaint dot;
      dot.setAntiAlias(true);
      dot.setColor("#1a1a1a"_color);
      canvas.drawCircle(cx, cy, ir, dot);
    }

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xD01, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0xD90);

      // The Window instrument: clip markers bracketing where the dots live, over the live
      // source histogram. lo = clip_black, hi = 255 - clip_white, on the real 0..255 axis.
      SkRect band = RPX(WindowM());
      ui::leptonica::DrawWindow(
          canvas, band, (float)clip_black, (float)(255 - clip_white), 0.f, 255.f,
          histogram.empty() ? nullptr : histogram.data(), max_log_count,
          dragging_marker == 0 ? slop::State::Pressed : slop::State::Default,
          dragging_marker == 1 ? slop::State::Pressed : slop::State::Default, 0xD10);
      slop::DrawText(canvas, "SOLID", P(WindowM().left - 0.02_cm, WindowM().bottom - 0.5_cm), 12.f,
                     kLabelInk, false, 0);
      slop::DrawText(canvas, "DOTS BETWEEN", P(WindowM().left + 1.1_cm, WindowM().bottom - 0.5_cm),
                     12.f, kLabelInk, false, 0);
      slop::DrawText(canvas, "SOLID", P(WindowM().right - 0.55_cm, WindowM().bottom - 0.5_cm), 12.f,
                     kLabelInk, false, 0);

      std::string title = "DITHER";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -2.9_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xD20);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -3.16_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0xD21);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint ppx{pos.x / kPxToMetric, -pos.y / kPxToMetric};
      Rect wm = WindowM();
      SkRect band = SkRect::MakeLTRB(wm.left / kPxToMetric, -wm.top / kPxToMetric,
                                     wm.right / kPxToMetric, -wm.bottom / kPxToMetric);
      bool lo_grab = ui::leptonica::LevelGrabsMarker(band, ppx, (float)clip_black, 0.f, 255.f);
      bool hi_grab =
          ui::leptonica::LevelGrabsMarker(band, ppx, (float)(255 - clip_white), 0.f, 255.f);
      if (lo_grab || hi_grab) {
        int which;
        if (lo_grab && hi_grab) {
          // Both in reach: take the nearer marker.
          float lox = ui::leptonica::LevelValueToX(band, (float)clip_black, 0.f, 255.f);
          float hix = ui::leptonica::LevelValueToX(band, (float)(255 - clip_white), 0.f, 255.f);
          which = std::abs(ppx.fX - lox) <= std::abs(ppx.fX - hix) ? 0 : 1;
        } else {
          which = lo_grab ? 0 : 1;
        }
        return std::make_unique<DitherClipDrag>(p, *this, which);
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

DitherClipDrag::DitherClipDrag(ui::Pointer& p, DitherToy& w, int which)
    : Action(p), widget(&w), which(which) {
  if (widget) widget->dragging_marker = which;
}
DitherClipDrag::~DitherClipDrag() {
  if (widget) {
    widget->dragging_marker = -1;
    widget->WakeAnimation();
  }
}
void DitherClipDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect wm = widget->WindowM();
  SkRect band = SkRect::MakeLTRB(wm.left / kPxToMetric, -wm.top / kPxToMetric,
                                 wm.right / kPxToMetric, -wm.bottom / kPxToMetric);
  float v = ui::leptonica::LevelXToValue(band, pos.x / kPxToMetric, 0.f, 255.f);
  if (auto t = widget->LockDither()) {
    {
      auto lock = std::lock_guard(t->mutex);
      if (which == 0)
        t->clip_black = std::clamp((int)std::lround(v), 0, 64);
      else
        t->clip_white = std::clamp(255 - (int)std::lround(v), 0, 64);
    }
    t->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Dither::MakeToy(ui::Widget* parent) {
  return std::make_unique<DitherToy>(parent, *this);
}

// ============================================================================
// Deskew
// ============================================================================
// pixFindSkewAndDeskew measures the text skew and rotates the input straight.
// Below leptonica's floors (confidence 3.0, angle 0.1 deg) it returns the
// input unchanged; the toy surfaces the refusal.
Pix* Deskew::ApplyOp(Pix* in, const float*) const {
  int m;
  {
    auto lock = std::lock_guard(mutex);
    m = mode;
  }
  l_float32 angle = 0.f, conf = 0.f;
  if (m == 0) return pixFindSkewAndDeskew(in, 0, &angle, &conf);
  // ANY orientation: sweep +-47 deg around both 0 and 90 deg; the returned angle includes the
  // orthogonal component. Same confidence floor (3.0) as leptonica's own pixDeskew.
  Pix* b = pixConvertTo1(in, 130);
  if (!b) return nullptr;
  int ret = pixFindSkewOrthogonalRange(b, &angle, &conf, 2, 2, 47.f, 1.f, 0.01f, 0.f);
  pixDestroy(&b);
  // NB: this fn's confidence is the margin over the COMPETING orientation - systematically lower
  // than pixFindSkew's ratio; its calibrated floor is conf > 0 (as the upstream usage), not 3.0.
  if (ret || conf <= 0.f || std::abs(angle) < 0.1f) return pixClone(in);
  return pixRotate(in, angle * 3.14159265f / 180.f, L_ROTATE_AREA_MAP, L_BRING_IN_WHITE, 0, 0);
}
void Deskew::PushFix(double deg) {
  if (auto target = fix_out->ObjectOrNull()) {
    char buf[24];
    snprintf(buf, sizeof(buf), "%.2f", deg);
    target->SetText(buf);
  }
}

void Deskew::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("mode");
  writer.Int(mode);
}
bool Deskew::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "mode") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) mode = std::clamp(v, 0, 1);
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct DeskewToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  float found_angle = 0.f;  // the correction angle pixFindSkew reports, degrees
  float found_conf = 0.f;
  bool has_measure = false;
  int mode = 0;
  uintptr_t last_push_target = 0;
  int last_pushed_centi = INT_MIN;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;
  constexpr static float kBottom = -4.15_cm;
  constexpr static float kTabL = -0.6_cm, kTabR = 1.1_cm;
  constexpr static float kTabB = 0.93_cm, kTabT = 2.05_cm;
  constexpr static float kGhostDeg = 18.f;
  constexpr static float kSweepDeg = 7.f;  // leptonica DefaultSweepRange - the gauge's real span

  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  DeskewToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockObject<Deskew>()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      mode = t->mode;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.85_cm, -1.25_cm, 1.95_cm, 0.75_cm); }
  Rect ModeCellM(int which) const {
    return which == 0 ? Rect(2.1_cm, 0.18_cm, 2.95_cm, 0.78_cm)
                      : Rect(2.1_cm, -0.5_cm, 2.95_cm, 0.1_cm);
  }
  Rect GaugeM() const { return Rect(-2.7_cm, -2.12_cm, 1.5_cm, -1.74_cm); }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    Rect tab = Rect(kTabL, kTabB, kTabR, kTabT);
    b.addRect(tab.sk);
    SkPath ghost = SkPath::Rect(tab.sk);
    b.addPath(ghost.makeTransform(SkMatrix::RotateDeg(kGhostDeg, {kTabR, kTabB})));
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kTabT).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, kTabT + 0.8_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.0_cm}, .dir = 180_deg};
    // FIX is a data output: left edge, below the Image input (the default would
    // stack it on the Next nub).
    if (&arg == static_cast<const Interface::Table*>(&Deskew::fix_out_tbl))
      return {.pos = {-kHalfW, GaugeM().CenterY()}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    has_measure = false;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    // Skew detection needs resolution; keep up to 1000 px and never upscale.
    float scl = std::min(1.f, std::min(1000.f / sw, 1000.f / sh));
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    l_float32 angle = 0.f, conf = 0.f;
    Pix* result = nullptr;
    if (mode == 0) {
      result = pixFindSkewAndDeskew(pix, 0, &angle, &conf);
    } else {
      Pix* b = pixConvertTo1(pix, 130);
      if (b) {
        int ret = pixFindSkewOrthogonalRange(b, &angle, &conf, 2, 2, 47.f, 1.f, 0.01f, 0.f);
        pixDestroy(&b);
        if (ret) conf = 0.f;
      }
      if (conf > 0.f && std::abs(angle) >= 0.1f) {
        result =
            pixRotate(pix, angle * 3.14159265f / 180.f, L_ROTATE_AREA_MAP, L_BRING_IN_WHITE, 0, 0);
      } else {
        result = pixClone(pix);
      }
    }
    pixDestroy(&pix);
    found_angle = (float)angle;
    found_conf = (float)conf;
    has_measure = true;
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockObject<Deskew>()) {
      int m;
      {
        auto lock = std::lock_guard(t->mutex);
        m = t->mode;
      }
      if (m != mode) {
        mode = m;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      // Re-push when the connected Number (or the found angle) changes - the link may resolve
      // only after the first measurement.
      if (has_measure) {
        // The port carries the fix Deskew would APPLY: below the trust floor leptonica refuses
        // (the gauge says UNCHANGED), so the honest fix is zero - same condition as the banner.
        bool trust = (mode == 1) ? found_conf > 0.f : found_conf >= 3.f;
        float fix = trust ? found_angle : 0.f;
        uintptr_t tgt = 0;
        if (auto target = t->fix_out->ObjectOrNull()) tgt = (uintptr_t)&*target;
        int centi = (int)std::lround(fix * 100.f);
        if (tgt != 0 && (tgt != last_push_target || centi != last_pushed_centi)) {
          t->PushFix(fix);
          last_push_target = tgt;
          last_pushed_centi = centi;
        }
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    Rect tab = Rect(kTabL, kTabB, kTabR, kTabT);
    {
      SkPaint page;
      page.setAntiAlias(true);
      page.setColor(SK_ColorWHITE);
      canvas.drawRect(
          Rect(tab.left + 0.08_cm, tab.bottom, tab.right - 0.08_cm, tab.top - 0.1_cm).sk, page);
      SkPaint line;
      line.setAntiAlias(true);
      line.setColor("#8e8676"_color);
      for (int i = 0; i < 4; ++i) {
        float y = tab.top - 0.32_cm - i * 0.22_cm;
        canvas.drawRect(Rect(tab.left + 0.22_cm, y - 0.045_cm, tab.right - 0.25_cm, y).sk, line);
      }
    }

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto PX = [&](float m) { return m / kPxToMetric; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xDE01, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0xDE90);

      {
        SkPoint pivot = P(kTabR, kTabB);
        canvas.save();
        canvas.rotate(-kGhostDeg, pivot.fX, pivot.fY);
        slop::HatchRect(canvas, RPX(tab), slop::kInkSoft, 9.f, 0xDE10);
        canvas.restore();
        for (int a = 0; a < 2; ++a) {
          float r = PX(1.55_cm - a * 0.3_cm);
          SkPathBuilder arc;
          for (int i = 0; i <= 8; ++i) {
            float deg = 124.f + (kGhostDeg + 4.f) * (float)i / 8.f;
            float rad = deg * 3.14159265f / 180.f;
            SkPoint pt{pivot.fX + r * std::cos(rad), pivot.fY - r * std::sin(rad)};
            if (i == 0)
              arc.moveTo(pt);
            else
              arc.lineTo(pt);
          }
          slop::SketchyStroke(canvas, arc.detach(), slop::kInkSoft, slop::kStrokeHair,
                              0xDE20u + (uint32_t)a, 1);
        }
      }

      const float sweep = mode == 1 ? 135.f : kSweepDeg;
      const int tick_step = mode == 1 ? 15 : 1;
      SkRect gauge = RPX(GaugeM());
      float gh = gauge.height();
      {
        SkPaint gp;
        gp.setAntiAlias(true);
        gp.setColor("#efe9da"_color);
        canvas.drawRect(gauge, gp);
        slop::SketchyStroke(canvas, slop::WobbleRect(gauge, slop::kWonk * 0.5f, slop::kSeg, 0xDE31),
                            slop::kInk, slop::kStrokeHair, 0xDE32, 1);
        int n = (int)sweep / tick_step;
        for (int i = -n; i <= n; ++i) {
          int d = i * tick_step;
          float x = gauge.fLeft + (d + sweep) / (2 * sweep) * gauge.width();
          bool major = d == 0 || (mode == 1 && (d == 90 || d == -90));
          float tick_h = major ? gh * 0.85f : gh * 0.4f;
          canvas.drawPath(slop::WobbleLine({x, gauge.fBottom}, {x, gauge.fBottom - tick_h}, 0.8f,
                                           6.f, 0xDE40u + (uint32_t)(i + n)),
                          slop::InkPaint(major ? slop::kInk : slop::kInkSoft, slop::kStrokeHair));
        }
        char lo[8], hi[8];
        snprintf(lo, sizeof(lo), "-%d", (int)sweep);
        snprintf(hi, sizeof(hi), "+%d", (int)sweep);
        slop::DrawText(canvas, lo, {gauge.fLeft - 26.f, gauge.fBottom}, 12.f, kLabelInk, false, 0);
        slop::DrawText(canvas, hi, {gauge.fRight + 4.f, gauge.fBottom}, 12.f, kLabelInk, false, 0);
      }
      slop::DrawText(canvas, "MEASURED SKEW", P(GaugeM().left, GaugeM().top + 0.14_cm), 15.f,
                     kLabelInk, false, 0);

      slop::DrawText(canvas, "SEARCH", P(ModeCellM(0).left, ModeCellM(0).top + 0.14_cm), 13.f,
                     kLabelInk, false, 0);
      for (int which = 0; which < 2; ++which) {
        SkRect cell = RPX(ModeCellM(which));
        bool sel = (mode == which);
        SkPath cp = slop::WonkyRoundRect(cell, cell.height() * 0.18f, slop::kWonk * 0.6f,
                                         0xDEA0u + (uint32_t)which);
        if (sel) slop::HandShadow(canvas, cp, {2.f, 2.f}, slop::kShadow, 0xDEA2u + (uint32_t)which);
        slop::MisregFill(canvas, cp, sel ? slop::kPaper : slop::kGray, 0xDEA4u + (uint32_t)which);
        slop::SketchyStroke(canvas, cp, slop::kInk, sel ? slop::kStroke : slop::kStrokeHair,
                            0xDEA6u + (uint32_t)which, sel ? 2 : 1);
        SkColor ink = sel ? slop::kInk : slop::kInkSoft;
        SkPoint c{cell.centerX() - cell.width() * 0.18f, cell.centerY()};
        float ph = cell.height() * 0.52f, pw = ph * 0.72f;
        canvas.save();
        canvas.rotate(which == 0 ? -8.f : -90.f, c.fX, c.fY);
        canvas.drawPath(slop::WobbleRect(SkRect::MakeXYWH(c.fX - pw / 2, c.fY - ph / 2, pw, ph),
                                         slop::kWonk * 0.5f, slop::kSeg, 0xDEA8u + (uint32_t)which),
                        slop::InkPaint(ink, slop::kStrokeHair));
        canvas.restore();
        const char* lab = which == 0 ? "7" : "90";
        slop::DrawText(canvas, lab, {cell.centerX() + cell.width() * 0.1f, cell.fBottom - 6.f},
                       cell.height() * 0.4f, ink, false, 0);
        if (sel) slop::Highlight(canvas, cell, slop::kBlue, 0xDEAAu + (uint32_t)which);
      }

      if (has_measure && (mode == 1 ? found_conf > 0.f : found_conf >= 3.f)) {
        float a = std::clamp(found_angle, -sweep, sweep);
        float x = gauge.fLeft + (a + sweep) / (2 * sweep) * gauge.width();
        canvas.drawPath(slop::WobbleLine({x, gauge.fBottom + gh * 0.25f},
                                         {x, gauge.fTop - gh * 0.3f}, 1.f, 7.f, 0xDE50),
                        slop::InkPaint(slop::kInk, slop::kStrokeBold));
        char buf[24];
        snprintf(buf, sizeof(buf), "%+.1f", found_angle);
        float fs = 14.f;
        float tw = slop::TextWidth(buf, fs);
        float cw = tw + 12.f, ch = fs * 1.5f;
        float cx = std::clamp(x, gauge.fLeft + cw / 2, gauge.fRight - cw / 2);
        float cy = gauge.fTop - gh * 0.55f - ch / 2;
        SkRect chip = SkRect::MakeXYWH(cx - cw / 2, cy - ch / 2, cw, ch);
        SkPath cp = slop::WonkyRoundRect(chip, ch * 0.35f, slop::kWonk, 0xDE51);
        slop::HandShadow(canvas, cp, {2.f, 2.f}, slop::kShadow, 0xDE52);
        slop::MisregFill(canvas, cp, slop::kPaper, 0xDE53);
        slop::SketchyStroke(canvas, cp, slop::kInk, slop::kStroke, 0xDE54, 1);
        slop::DrawText(canvas, buf, {cx - tw / 2, cy + fs * 0.36f}, fs, slop::kInk, false, 0);
      } else if (has_measure) {
        slop::Badge(canvas, {gauge.centerX(), gauge.centerY()}, "LOW CONF - UNCHANGED", slop::kRed,
                    -4.f, 0xDE60);
      }
      {
        char buf[24];
        if (has_measure)
          snprintf(buf, sizeof(buf), "CONF %.1f", found_conf);
        else
          snprintf(buf, sizeof(buf), "CONF -");
        slop::DrawText(canvas, buf, P(GaugeM().left, GaugeM().bottom - 0.42_cm), 13.f, kLabelInk,
                       false, 0);
      }

      std::string title = "DESKEW";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -3.0_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xDE70);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -3.28_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0xDE71);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      for (int which = 0; which < 2; ++which) {
        Rect r = ModeCellM(which);
        if (pos.x >= r.left && pos.x <= r.right && pos.y >= r.bottom && pos.y <= r.top) {
          if (auto t = LockObject<Deskew>()) {
            {
              auto lock = std::lock_guard(t->mutex);
              t->mode = which;
            }
            t->WakeToys();
          }
          return std::make_unique<QuantPoke>(p);  // momentary poke (declared earlier)
        }
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

std::unique_ptr<ObjectToy> Deskew::MakeToy(ui::Widget* parent) {
  return std::make_unique<DeskewToy>(parent, *this);
}

// ============================================================================
// Find Level
// ============================================================================
// pixSplitDistributionFgBg on the luminance histogram; the toy measures the
// live proxy, Measure (RUN) the full resolution.
static bool FindLevelMeasurePix(Pix* pix32, float scorefract, int& thresh, int& fg, int& bg) {
  Pix* gray = pixConvertRGBToLuminance(pix32);
  if (!gray) return false;
  l_int32 th = 0, fgval = 0, bgval = 0;
  int ret = pixSplitDistributionFgBg(gray, scorefract, 1, &th, &fgval, &bgval, nullptr);
  pixDestroy(&gray);
  if (ret) return false;
  thresh = th;
  fg = fgval;
  bg = bgval;
  return true;
}

void FindLevel::PushLevel(double v) {
  if (auto target = level->ObjectOrNull()) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)std::lround(v));
    target->SetText(buf);
  }
}

void FindLevel::Measure() {
  auto nested = image->FindInterface();
  Object* owner = nested.Owner<Object>();
  ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
  sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
  if (!source) return;
  Pix* pix = SkImageToPix(source);
  if (!pix) return;
  float sf;
  {
    auto lock = std::lock_guard(mutex);
    sf = std::clamp(scorefract, 0.f, 0.5f);
  }
  int th = 0, fg = 0, bg = 0;
  bool ok = FindLevelMeasurePix(pix, sf, th, fg, bg);
  pixDestroy(&pix);
  if (!ok) return;
  {
    auto lock = std::lock_guard(mutex);
    last_thresh = th;
    last_fg = fg;
    last_bg = bg;
  }
  PushLevel(th);
  WakeToys();
}

void FindLevel::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("scorefract");
  writer.Double(scorefract);
}
bool FindLevel::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "scorefract") {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) scorefract = std::clamp((float)x, 0.f, 0.5f);
    return true;
  }
  return false;
}

struct FindLevelToy;
struct FindLevelFractDrag : Action {
  TrackedPtr<FindLevelToy> widget;
  FindLevelFractDrag(ui::Pointer& p, FindLevelToy& w);
  ~FindLevelFractDrag();
  void Update() override;
};

struct FindLevelToy : ObjectToy {
  std::vector<uint32_t> histogram;
  float max_log_count = 1.0f;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  bool has_measure = false;

  int found_thresh = 128, found_fg = 60, found_bg = 200;
  float scorefract = 0.1f;
  bool dragging = false;
  uintptr_t last_push_target = 0;
  int last_pushed = -1;

  std::unique_ptr<ui::slop::RunButton> glass;


  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;
  constexpr static float kBottom = -4.00_cm;

  Ptr<FindLevel> LockFind() const { return LockObject<FindLevel>(); }

  FindLevelToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockFind()) t->measure->ScheduleRun();
    });
    if (auto t = LockFind()) {
      auto lock = std::lock_guard(t->mutex);
      scorefract = t->scorefract;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect BandM() const { return Rect(-2.7_cm, -1.05_cm, 2.7_cm, 0.35_cm); }
  Rect FractSliderM() const { return Rect(-2.7_cm, -2.1_cm, 0.7_cm, -1.75_cm); }

  // Flag x within the mound span for a 0..255 level.
  float FlagX(int level) const { return -2.4_cm + (float)level / 255.f * 4.8_cm; }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    b.addOval(Rect(-2.75_cm, kBodyTop - 0.4_cm, -0.15_cm, 1.95_cm).sk);  // fg mound
    b.addOval(Rect(0.15_cm, kBodyTop - 0.4_cm, 2.75_cm, 2.25_cm).sk);    // bg mound
    float fx = FlagX(found_thresh);
    b.addRect(Rect(fx - 0.06_cm, kBodyTop - 0.1_cm, fx + 0.06_cm, 2.85_cm).sk);  // pole
    SkPathBuilder flag;
    flag.moveTo(fx + 0.04_cm, 2.85_cm);
    flag.lineTo(fx + 0.75_cm, 2.62_cm);
    flag.lineTo(fx + 0.04_cm, 2.4_cm);
    flag.close();
    b.addPath(flag.detach());
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, 2.85_cm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, 3.3_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&FindLevel::image_tbl))
      return {.pos = {-kHalfW, 0.5_cm}, .dir = 180_deg};
    if (&arg == static_cast<const Interface::Table*>(&FindLevel::level_tbl))
      return {.pos = {-kHalfW, -0.5_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  void RecomputeMeasure() {
    auto tool = LockFind();
    histogram.assign(256, 0);
    max_log_count = 1.0f;
    has_measure = false;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(1.f, std::min(400.f / sw, 400.f / sh));
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    {
      int w = pixGetWidth(pix), h = pixGetHeight(pix);
      l_int32 wpl = pixGetWpl(pix);
      l_uint32* d = pixGetData(pix);
      for (int y = 0; y < h; ++y) {
        l_uint32* line = d + (size_t)y * wpl;
        for (int x = 0; x < w; ++x) {
          uint32_t rgba = __builtin_bswap32(line[x]);
          float r = (rgba >> 24) & 0xff, gg = (rgba >> 16) & 0xff, b = (rgba >> 8) & 0xff;
          int lum = (int)(0.3f * r + 0.59f * gg + 0.11f * b);
          histogram[std::clamp(lum, 0, 255)]++;
        }
      }
      for (uint32_t c : histogram)
        if (c > 0) max_log_count = std::max(max_log_count, std::log((float)c + 1.f));
    }
    float sf;
    {
      auto lock = std::lock_guard(tool->mutex);
      sf = std::clamp(tool->scorefract, 0.f, 0.5f);
    }
    int th = 0, fg = 0, bg = 0;
    bool ok = FindLevelMeasurePix(pix, sf, th, fg, bg);
    pixDestroy(&pix);
    if (!ok) return;
    found_thresh = th;
    found_fg = fg;
    found_bg = bg;
    has_measure = true;
    {
      auto lock = std::lock_guard(tool->mutex);
      tool->last_thresh = th;
      tool->last_fg = fg;
      tool->last_bg = bg;
    }
    tool->PushLevel(th);  // the live cable: the connected Number tracks the measurement
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockFind()) {
      float sf;
      {
        auto lock = std::lock_guard(t->mutex);
        sf = t->scorefract;
      }
      if (sf != scorefract) {
        scorefract = sf;
        preview_dirty = true;
      }
      uint32_t id = 0;
      {
        auto nested = t->image->FindInterface();
        Object* owner = nested.Owner<Object>();
        ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
        sk_sp<SkImage> img = ip ? ip.GetImage() : nullptr;
        id = SourceImageId(img);
      }
      if (id != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputeMeasure();
      // Re-push when the connected Number (or the value) changes - the link may resolve only
      // after the first measurement. (has_source stays true even with no source: this toy keeps
      // ticking while unconnected, because nothing wakes it when the Photo link lands - the toy
      // slept through deserialization's link pass otherwise.)
      if (has_measure) {
        uintptr_t tgt = 0;
        if (auto target = t->level->ObjectOrNull()) tgt = (uintptr_t)&*target;
        if (tgt != 0 && (tgt != last_push_target || found_thresh != last_pushed)) {
          t->PushLevel(found_thresh);
          last_push_target = tgt;
          last_pushed = found_thresh;
        }
      }
      has_source = true;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    {
      SkPaint fgp;
      fgp.setAntiAlias(true);
      int v = std::clamp(found_fg, 0, 255);
      fgp.setColor(SkColorSetRGB(v, v, v));
      canvas.drawOval(Rect(-2.6_cm, kBodyTop - 0.35_cm, -0.3_cm, 1.78_cm).sk, fgp);
      SkPaint bgp;
      bgp.setAntiAlias(true);
      v = std::clamp(found_bg, 0, 255);
      bgp.setColor(SkColorSetRGB(v, v, v));
      canvas.drawOval(Rect(0.3_cm, kBodyTop - 0.35_cm, 2.6_cm, 2.07_cm).sk, bgp);
    }
    {
      float fx = FlagX(found_thresh);
      SkPaint fp;
      fp.setAntiAlias(true);
      fp.setColor("#ed1c24"_color);
      SkPathBuilder flag;
      flag.moveTo(fx + 0.06_cm, 2.81_cm);
      flag.lineTo(fx + 0.68_cm, 2.62_cm);
      flag.lineTo(fx + 0.06_cm, 2.44_cm);
      flag.close();
      canvas.drawPath(flag.detach(), fp);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xFD01, 1);

      ui::leptonica::DrawLevel(canvas, RPX(BandM()), (float)found_thresh, 0.f, 255.f,
                               histogram.empty() ? nullptr : histogram.data(), max_log_count, true,
                               false, slop::State::Disabled, 0xFD10);
      slop::DrawText(canvas, "FOUND LEVEL", P(BandM().left, BandM().top + 0.5_cm), 15.f, kLabelInk,
                     false, 0);

      {
        char buf[16];
        SkRect fgs = RPX(Rect(-2.7_cm, -1.55_cm, -2.3_cm, -1.25_cm));
        SkPaint sp;
        sp.setAntiAlias(true);
        int v = std::clamp(found_fg, 0, 255);
        sp.setColor(SkColorSetRGB(v, v, v));
        canvas.drawRect(fgs, sp);
        canvas.drawPath(slop::WobbleRect(fgs, slop::kWonk * 0.5f, slop::kSeg, 0xFD20),
                        slop::InkPaint(slop::kInk, slop::kStrokeHair));
        snprintf(buf, sizeof(buf), "FG %d", v);
        slop::DrawText(canvas, buf, {fgs.fRight + 6.f, fgs.fBottom - 2.f}, 13.f, kLabelInk, false,
                       0);
        SkRect bgs = RPX(Rect(-0.9_cm, -1.55_cm, -0.5_cm, -1.25_cm));
        int w2 = std::clamp(found_bg, 0, 255);
        sp.setColor(SkColorSetRGB(w2, w2, w2));
        canvas.drawRect(bgs, sp);
        canvas.drawPath(slop::WobbleRect(bgs, slop::kWonk * 0.5f, slop::kSeg, 0xFD21),
                        slop::InkPaint(slop::kInk, slop::kStrokeHair));
        snprintf(buf, sizeof(buf), "BG %d", w2);
        slop::DrawText(canvas, buf, {bgs.fRight + 6.f, bgs.fBottom - 2.f}, 13.f, kLabelInk, false,
                       0);
      }

      Rect fs = FractSliderM();
      slop::Slider(canvas, RPX(fs), std::clamp(scorefract / 0.5f, 0.f, 1.f), slop::State::Default,
                   0xFD30);
      {
        char buf[28];
        snprintf(buf, sizeof(buf), "SCOREFRACT %.2f", scorefract);
        slop::DrawText(canvas, buf, P(fs.left, fs.top + 0.14_cm), 15.f, kLabelInk, false, 0);
      }

      std::string title = "FIND LEVEL";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -2.85_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xFD40);
      std::string credit = "pixSplitDistributionFgBg()";
      float fw = slop::TextWidth(credit, kCreditTextPx);
      SkPoint fc = P(0, -3.13_cm);
      slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                     false, 0xFD41);
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      Rect fs = FractSliderM();
      if (pos.x >= fs.left - 0.2_cm && pos.x <= fs.right + 0.2_cm && pos.y >= fs.bottom - 0.2_cm &&
          pos.y <= fs.top + 0.2_cm)
        return std::make_unique<FindLevelFractDrag>(p, *this);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

FindLevelFractDrag::FindLevelFractDrag(ui::Pointer& p, FindLevelToy& w) : Action(p), widget(&w) {
  widget->dragging = true;
}
FindLevelFractDrag::~FindLevelFractDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void FindLevelFractDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = widget->FractSliderM();
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  if (auto f = widget->LockFind()) {
    {
      auto lock = std::lock_guard(f->mutex);
      f->scorefract = t * 0.5f;
    }
    f->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> FindLevel::MakeToy(ui::Widget* parent) {
  return std::make_unique<FindLevelToy>(parent, *this);
}

// ============================================================================
// Count
// ============================================================================
// Count the connected components of the binarized page; the connectivity
// chooser changes the answer (diagonal touches join under 8).
void Count::PushCount(int n) {
  if (auto target = count->ObjectOrNull()) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", n);
    target->SetText(buf);
  }
}

void Count::Measure() {
  auto nested = image->FindInterface();
  Object* owner = nested.Owner<Object>();
  ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
  sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
  if (!source) return;
  Pix* pix = SkImageToPix(source);
  if (!pix) return;
  int conn;
  {
    auto lock = std::lock_guard(mutex);
    conn = eight ? 8 : 4;
  }
  Pix* bin = pixConvertTo1(pix, 130);
  pixDestroy(&pix);
  if (!bin) return;
  l_int32 n = 0;
  int ret = pixCountConnComp(bin, conn, &n);
  pixDestroy(&bin);
  if (ret) return;
  {
    auto lock = std::lock_guard(mutex);
    last_count = n;
  }
  PushCount(n);
  WakeToys();
}

void Count::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("connectivity");
  writer.Int(eight ? 8 : 4);
}
bool Count::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "connectivity") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) eight = (v != 4);
    return true;
  }
  return false;
}

struct CountToy;
struct CountConnPoke : Action {
  CountConnPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct CountToy : ObjectToy {
  sk_sp<SkImage> cached_preview;  // binarized blobs, muted (what is being counted)
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  bool has_measure = false;

  int found_count = 0;
  bool eight = true;
  uintptr_t last_push_target = 0;
  int last_pushed = -1;

  std::unique_ptr<ui::slop::RunButton> glass;


  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;
  constexpr static float kBottom = -4.00_cm;

  Ptr<Count> LockCount() const { return LockObject<Count>(); }

  CountToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockCount()) t->measure->ScheduleRun();
    });
    if (auto t = LockCount()) {
      auto lock = std::lock_guard(t->mutex);
      eight = t->eight;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.85_cm, -1.3_cm, 2.85_cm, 0.7_cm); }
  Rect ConnM() const { return Rect(-2.7_cm, -2.35_cm, -1.05_cm, -1.7_cm); }
  // Reaches down past kBodyTop so the chip is one contour with the body (offset outlines).
  Rect TotalChipM() const { return Rect(1.1_cm, 0.88_cm, 2.7_cm, 2.05_cm); }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    for (int i = 0; i < 4; ++i) {
      float x = -1.45_cm + i * 0.55_cm;
      b.addRect(Rect(x, 0.9_cm, x + 0.26_cm, 2.2_cm).sk);
    }
    SkPath diag = SkPath::Rect(Rect(-0.42_cm, 1.42_cm, 1.78_cm, 1.66_cm).sk);
    b.addPath(diag.makeTransform(SkMatrix::RotateDeg(-32.f, {-0.4_cm, 1.55_cm})));
    b.addRect(TotalChipM().sk);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, 2.2_cm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, 2.7_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&Count::image_tbl))
      return {.pos = {-kHalfW, 0.5_cm}, .dir = 180_deg};
    if (&arg == static_cast<const Interface::Table*>(&Count::count_tbl))
      return {.pos = {-kHalfW, -0.5_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  void RecomputeMeasure() {
    auto tool = LockCount();
    cached_preview = nullptr;
    has_measure = false;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    // Cap 800, never upscale: resampling changes topology and would lie about
    // the count.
    float scl = std::min(1.f, std::min(800.f / sw, 800.f / sh));
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    int conn;
    {
      auto lock = std::lock_guard(tool->mutex);
      conn = tool->eight ? 8 : 4;
    }
    Pix* bin = pixConvertTo1(pix, 130);
    pixDestroy(&pix);
    if (!bin) return;
    l_int32 n = 0;
    int ret = pixCountConnComp(bin, conn, &n);
    if (!ret) {
      found_count = n;
      has_measure = true;
      auto lock = std::lock_guard(tool->mutex);
      tool->last_count = n;
    }
    Pix* disp = pixCreate(pixGetWidth(bin), pixGetHeight(bin), 32);
    if (disp) {
      pixSetAll(disp);
      l_uint32 gray = 0;
      composeRGBPixel(0x8E, 0x86, 0x76, &gray);
      pixSetMasked(disp, bin, gray);
      cached_preview = PixToSkImage(disp);
      pixDestroy(&disp);
    }
    pixDestroy(&bin);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    if (auto t = LockCount()) {
      bool e;
      {
        auto lock = std::lock_guard(t->mutex);
        e = t->eight;
      }
      if (e != eight) {
        eight = e;
        preview_dirty = true;
      }
      uint32_t id = 0;
      {
        auto nested = t->image->FindInterface();
        Object* owner = nested.Owner<Object>();
        ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
        sk_sp<SkImage> img = ip ? ip.GetImage() : nullptr;
        id = SourceImageId(img);
      }
      if (id != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputeMeasure();
      if (has_measure) {
        uintptr_t tgt = 0;
        if (auto target = t->count->ObjectOrNull()) tgt = (uintptr_t)&*target;
        if (tgt != 0 && (tgt != last_push_target || found_count != last_pushed)) {
          t->PushCount(found_count);
          last_push_target = tgt;
          last_pushed = found_count;
        }
      }
    }
    return animation::Animating;  // finder: keep ticking while unconnected (the FindLevel lesson)
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xC001, 1);

      {
        SkRect chip = RPX(TotalChipM());
        slop::MisregFill(canvas, slop::WonkyRoundRect(chip, 8.f, slop::kWonk, 0xC010), slop::kPaper,
                         0xC011);
        slop::SketchyStroke(canvas, slop::WonkyRoundRect(chip, 8.f, slop::kWonk, 0xC010),
                            slop::kInk, slop::kStroke, 0xC012, 2);
        char buf[16];
        if (has_measure)
          snprintf(buf, sizeof(buf), "%d", found_count);
        else
          snprintf(buf, sizeof(buf), "-");
        float fs = 40.f;
        float tw = slop::TextWidth(buf, fs);
        slop::DrawText(canvas, buf, {chip.centerX() - tw / 2, chip.centerY() + fs * 0.36f}, fs,
                       slop::kInk, false, 0);
      }

      ui::leptonica::DrawConnectivity(canvas, RPX(ConnM()), eight, slop::State::Default, 0xC020);
      slop::DrawText(canvas, "CONNECTIVITY", P(ConnM().left, ConnM().top + 0.14_cm), 13.f,
                     kLabelInk, false, 0);

      std::string title = "COUNT";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -2.85_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xC030);
      std::string credit = "pixCountConnComp()";
      float fw = slop::TextWidth(credit, kCreditTextPx);
      SkPoint fc = P(0, -3.13_cm);
      slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                     false, 0xC031);
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint ppx{pos.x / kPxToMetric, -pos.y / kPxToMetric};
      Rect cm = ConnM();
      SkRect cellr = SkRect::MakeLTRB(cm.left / kPxToMetric, -cm.top / kPxToMetric,
                                      cm.right / kPxToMetric, -cm.bottom / kPxToMetric);
      int hit = ui::leptonica::ConnectivityHit(cellr, ppx);
      if (hit != 0) {
        if (auto t = LockCount()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->eight = hit == 8;
          }
          t->WakeToys();
        }
        return std::make_unique<CountConnPoke>(p);
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

std::unique_ptr<ObjectToy> Count::MakeToy(ui::Widget* parent) {
  return std::make_unique<CountToy>(parent, *this);
}

// ============================================================================
// Select
// ============================================================================
Pix* Select::ApplyOp(Pix* in, const float*) const {
  int ax, l, h, l2, h2;
  bool ins;
  {
    auto lock = std::lock_guard(mutex);
    ax = std::clamp(axes, 0, 3);
    int amax = (ax == 1 || ax == 2) ? 239 : 255;  // leptonica hue tops out at 239
    l = std::clamp(lo, 0, amax);
    h = std::clamp(hi, l, amax);
    l2 = std::clamp(lo2, 0, 255);
    h2 = std::clamp(hi2, l2, 255);
    ins = inside;
  }
  if (ax == 0) {
    Pix* g = pixConvertTo8(in, 0);
    if (!g) return nullptr;
    Pix* out = pixGenerateMaskByBand(g, l, h, ins ? 1 : 0, 0);
    pixDestroy(&g);
    return out;  // 1 bpp mask
  }
  // 2-D HSV band: center+halfwidth per axis, conjunction of both bands.
  Pix* rgb = in;
  bool owned = false;
  if (pixGetDepth(in) != 32 || pixGetColormap(in)) {
    rgb = pixConvertTo32(in);
    owned = true;
    if (!rgb) return nullptr;
  }
  int c1 = (l + h) / 2, hw1 = std::max(0, (h - l) / 2);
  int c2 = (l2 + h2) / 2, hw2 = std::max(0, (h2 - l2) / 2);
  int region = ins ? L_INCLUDE_REGION : L_EXCLUDE_REGION;
  Pix* out = nullptr;
  if (ax == 1)
    out = pixMakeRangeMaskHS(rgb, c1, hw1, c2, hw2, region);
  else if (ax == 2)
    out = pixMakeRangeMaskHV(rgb, c1, hw1, c2, hw2, region);
  else
    out = pixMakeRangeMaskSV(rgb, c1, hw1, c2, hw2, region);
  if (owned) pixDestroy(&rgb);
  return out;  // 1 bpp mask
}
void Select::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("axes");
  writer.Int(axes);
  writer.Key("lo");
  writer.Int(lo);
  writer.Key("hi");
  writer.Int(hi);
  writer.Key("lo2");
  writer.Int(lo2);
  writer.Key("hi2");
  writer.Int(hi2);
  writer.Key("inside");
  writer.Bool(inside);
}
bool Select::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "axes") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) axes = std::clamp(v, 0, 3);
  } else if (key == "lo") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) lo = std::clamp(v, 0, 255);
  } else if (key == "hi") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) hi = std::clamp(v, 0, 255);
  } else if (key == "lo2") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) lo2 = std::clamp(v, 0, 255);
  } else if (key == "hi2") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) hi2 = std::clamp(v, 0, 255);
  } else if (key == "inside") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) inside = v;
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct SelectToy;
struct SelectBandDrag : Action {
  TrackedPtr<SelectToy> widget;
  int which;  // 0 = lo, 1 = hi
  SelectBandDrag(ui::Pointer& p, SelectToy& w, int which);
  ~SelectBandDrag();
  void Update() override;
};
struct SelectPoke : Action {
  SelectPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct SelectToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  std::vector<uint32_t> histogram;
  std::vector<uint32_t> histogram2;
  float max_log_count = 1.0f;
  float max_log_count2 = 1.0f;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int axes = 0;
  int lo = 80, hi = 180;
  int lo2 = 0, hi2 = 255;
  bool inside = true;
  int dragging_marker = -1;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;
  constexpr static float kBottom = -4.80_cm;
  constexpr static float kPlateauTop = 1.9_cm;

  int AxisAMax() const { return (axes == 1 || axes == 2) ? 239 : 255; }
  const char* AxisAName() const { return axes == 0 ? "LUM" : (axes == 3 ? "SAT" : "HUE"); }
  const char* AxisBName() const { return axes == 1 ? "SAT" : "VAL"; }

  Ptr<Select> LockSel() const { return LockObject<Select>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  SelectToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockSel()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      axes = t->axes;
      lo = t->lo;
      hi = t->hi;
      lo2 = t->lo2;
      hi2 = t->hi2;
      inside = t->inside;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.85_cm, -1.05_cm, 2.85_cm, 0.75_cm); }
  Rect WindowM() const { return Rect(-2.7_cm, -2.05_cm, 1.5_cm, -1.55_cm); }
  Rect WindowBM() const { return Rect(-2.7_cm, -2.95_cm, 1.0_cm, -2.45_cm); }
  SkPoint WheelCM() const { return {2.15_cm, -2.5_cm}; }
  float WheelRM() const { return 0.36_cm; }
  Rect InChipM(int which) const {
    return which == 0 ? Rect(1.7_cm, -1.86_cm, 2.1_cm, -1.54_cm)
                      : Rect(2.2_cm, -1.86_cm, 2.6_cm, -1.54_cm);
  }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    float amax = (float)AxisAMax();
    float x0 = -2.8_cm + (float)lo / amax * 5.6_cm;
    float x1 = -2.8_cm + (float)hi / amax * 5.6_cm;
    if (x1 - x0 < 0.3_cm) x1 = x0 + 0.3_cm;
    b.addRect(Rect(x0, kBodyTop - 0.05_cm, x1, kPlateauTop).sk);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kPlateauTop).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, kPlateauTop + 0.5_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.0_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t SelHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockSel()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)t->axes;
      h *= 16777619u;
      h ^= (uint32_t)t->lo;
      h *= 16777619u;
      h ^= (uint32_t)t->hi;
      h *= 16777619u;
      h ^= (uint32_t)t->lo2;
      h *= 16777619u;
      h ^= (uint32_t)t->hi2;
      h *= 16777619u;
      h ^= (uint32_t)(t->inside ? 1 : 0);
      h *= 16777619u;
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    histogram.assign(256, 0);
    histogram2.assign(256, 0);
    max_log_count = 1.0f;
    max_log_count2 = 1.0f;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    {
      int w = pixGetWidth(pix), h = pixGetHeight(pix);
      l_int32 wpl = pixGetWpl(pix);
      l_uint32* d = pixGetData(pix);
      for (int y = 0; y < h; ++y) {
        l_uint32* line = d + (size_t)y * wpl;
        for (int x = 0; x < w; ++x) {
          uint32_t rgba = __builtin_bswap32(line[x]);
          int r = (rgba >> 24) & 0xff, gg = (rgba >> 16) & 0xff, b = (rgba >> 8) & 0xff;
          if (axes == 0) {
            int lum = (int)(0.3f * r + 0.59f * gg + 0.11f * b);
            histogram[std::clamp(lum, 0, 255)]++;
          } else {
            l_int32 hh = 0, ss = 0, vv = 0;
            convertRGBToHSV(r, gg, b, &hh, &ss, &vv);
            int aa = axes == 3 ? ss : hh;
            int bb = axes == 1 ? ss : vv;
            histogram[std::clamp(aa, 0, 255)]++;
            histogram2[std::clamp(bb, 0, 255)]++;
          }
        }
      }
      for (uint32_t c : histogram)
        if (c > 0) max_log_count = std::max(max_log_count, std::log((float)c + 1.f));
      for (uint32_t c : histogram2)
        if (c > 0) max_log_count2 = std::max(max_log_count2, std::log((float)c + 1.f));
    }
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockSel()) {
      uint32_t h = SelHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        if (axes != t->axes) preview_dirty = true;  // histograms are per-axis
        axes = t->axes;
        lo = t->lo;
        hi = t->hi;
        lo2 = t->lo2;
        hi2 = t->hi2;
        inside = t->inside;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    {
      float amax = (float)AxisAMax();
      float x0 = -2.8_cm + (float)lo / amax * 5.6_cm;
      float x1 = -2.8_cm + (float)hi / amax * 5.6_cm;
      if (x1 - x0 < 0.3_cm) x1 = x0 + 0.3_cm;
      int mid = std::clamp((lo + hi) / 2, 0, (int)amax);
      SkPaint pp;
      pp.setAntiAlias(true);
      if (axes == 1 || axes == 2) {
        SkScalar hsv[3] = {mid * 360.f / 240.f, 0.75f, 0.85f};
        pp.setColor(SkHSVToColor(hsv));
      } else
        pp.setColor(SkColorSetRGB(mid, mid, mid));
      canvas.drawRect(Rect(x0 + 0.12_cm, kBodyTop, x1 - 0.12_cm, kPlateauTop - 0.12_cm).sk, pp);
    }

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0x5E01, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0x5E90);

      ui::leptonica::DrawWindow(
          canvas, RPX(WindowM()), (float)lo, (float)hi, 0.f, (float)AxisAMax(),
          histogram.empty() ? nullptr : histogram.data(), max_log_count,
          dragging_marker == 0 ? slop::State::Pressed : slop::State::Default,
          dragging_marker == 1 ? slop::State::Pressed : slop::State::Default, 0x5E10);
      {
        char abuf[24];
        snprintf(abuf, sizeof(abuf), "KEEP %s BAND", AxisAName());
        slop::DrawText(canvas, abuf, P(WindowM().left, WindowM().top + 0.5_cm), 15.f, kLabelInk,
                       false, 0);
      }
      if (axes != 0) {
        ui::leptonica::DrawWindow(
            canvas, RPX(WindowBM()), (float)lo2, (float)hi2, 0.f, 255.f,
            histogram2.empty() ? nullptr : histogram2.data(), max_log_count2,
            dragging_marker == 2 ? slop::State::Pressed : slop::State::Default,
            dragging_marker == 3 ? slop::State::Pressed : slop::State::Default, 0x5E18);
        char bbuf[24];
        snprintf(bbuf, sizeof(bbuf), "%s BAND", AxisBName());
        slop::DrawText(canvas, bbuf, P(WindowBM().left, WindowBM().top + 0.5_cm), 15.f, kLabelInk,
                       false, 0);
      }
      {
        const char* axlabels[] = {"LUM", "H\xc2\xb7S", "H\xc2\xb7V", "S\xc2\xb7V"};
        ui::leptonica::DrawModeWheel(
            canvas, {WheelCM().fX / kPxToMetric, -WheelCM().fY / kPxToMetric},
            WheelRM() / kPxToMetric, axlabels, 4, axes, slop::State::Default, 0x5E50);
      }

      for (int which = 0; which < 2; ++which) {
        SkRect chip = RPX(InChipM(which));
        bool sel = (which == 1) != inside;
        SkPath cp = slop::WonkyRoundRect(chip, chip.height() * 0.3f, slop::kWonk * 0.5f,
                                         0x5E20u + (uint32_t)which);
        if (sel) slop::HandShadow(canvas, cp, {2.f, 2.f}, slop::kShadow, 0x5E24u + (uint32_t)which);
        slop::MisregFill(canvas, cp, sel ? slop::kPaper : slop::kGray, 0x5E28u + (uint32_t)which);
        slop::SketchyStroke(canvas, cp, slop::kInk, sel ? slop::kStroke : slop::kStrokeHair,
                            0x5E2Cu + (uint32_t)which, 1);
        const char* lab = which == 0 ? "IN" : "OUT";
        float lw = slop::TextWidth(lab, 12.f);
        slop::DrawText(canvas, lab, {chip.centerX() - lw / 2, chip.centerY() + 4.f}, 12.f,
                       sel ? slop::kInk : slop::kInkSoft, false, 0);
        if (sel) slop::Highlight(canvas, chip, slop::kBlue, 0x5E30);
      }

      std::string title = "SELECT";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -3.6_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0x5E40);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -3.87_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0x5E41);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint ppx{pos.x / kPxToMetric, -pos.y / kPxToMetric};
      for (int which = 0; which < 2; ++which) {
        Rect r = InChipM(which);
        if (pos.x >= r.left && pos.x <= r.right && pos.y >= r.bottom && pos.y <= r.top) {
          if (auto t = LockSel()) {
            {
              auto lock = std::lock_guard(t->mutex);
              t->inside = which == 0;
            }
            t->WakeToys();
          }
          return std::make_unique<SelectPoke>(p);
        }
      }
      int whit =
          ui::leptonica::ModeWheelHit({WheelCM().fX / kPxToMetric, -WheelCM().fY / kPxToMetric},
                                      WheelRM() / kPxToMetric, ppx, 4);
      if (whit >= 0) {
        if (auto t = LockSel()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->axes = whit;
            int amax = (whit == 1 || whit == 2) ? 239 : 255;
            t->lo = std::min(t->lo, amax);
            t->hi = std::min(t->hi, amax);
          }
          t->WakeToys();
        }
        return std::make_unique<SelectPoke>(p);
      }
      Rect wm = WindowM();
      SkRect band = SkRect::MakeLTRB(wm.left / kPxToMetric, -wm.top / kPxToMetric,
                                     wm.right / kPxToMetric, -wm.bottom / kPxToMetric);
      float amax = (float)AxisAMax();
      bool lo_grab = ui::leptonica::LevelGrabsMarker(band, ppx, (float)lo, 0.f, amax);
      bool hi_grab = ui::leptonica::LevelGrabsMarker(band, ppx, (float)hi, 0.f, amax);
      if (lo_grab || hi_grab) {
        int which;
        if (lo_grab && hi_grab) {
          float lox = ui::leptonica::LevelValueToX(band, (float)lo, 0.f, amax);
          float hix = ui::leptonica::LevelValueToX(band, (float)hi, 0.f, amax);
          which = std::abs(ppx.fX - lox) <= std::abs(ppx.fX - hix) ? 0 : 1;
        } else {
          which = lo_grab ? 0 : 1;
        }
        return std::make_unique<SelectBandDrag>(p, *this, which);
      }
      if (axes != 0) {
        Rect wb = WindowBM();
        SkRect bandb = SkRect::MakeLTRB(wb.left / kPxToMetric, -wb.top / kPxToMetric,
                                        wb.right / kPxToMetric, -wb.bottom / kPxToMetric);
        bool lo2g = ui::leptonica::LevelGrabsMarker(bandb, ppx, (float)lo2, 0.f, 255.f);
        bool hi2g = ui::leptonica::LevelGrabsMarker(bandb, ppx, (float)hi2, 0.f, 255.f);
        if (lo2g || hi2g) {
          int which;
          if (lo2g && hi2g) {
            float lox = ui::leptonica::LevelValueToX(bandb, (float)lo2, 0.f, 255.f);
            float hix = ui::leptonica::LevelValueToX(bandb, (float)hi2, 0.f, 255.f);
            which = std::abs(ppx.fX - lox) <= std::abs(ppx.fX - hix) ? 2 : 3;
          } else {
            which = lo2g ? 2 : 3;
          }
          return std::make_unique<SelectBandDrag>(p, *this, which);
        }
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

SelectBandDrag::SelectBandDrag(ui::Pointer& p, SelectToy& w, int which)
    : Action(p), widget(&w), which(which) {
  widget->dragging_marker = which;
}
SelectBandDrag::~SelectBandDrag() {
  if (widget) {
    widget->dragging_marker = -1;
    widget->WakeAnimation();
  }
}
void SelectBandDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  bool axis_b = which >= 2;
  Rect wm = axis_b ? widget->WindowBM() : widget->WindowM();
  float vmax = axis_b ? 255.f : (float)widget->AxisAMax();
  SkRect band = SkRect::MakeLTRB(wm.left / kPxToMetric, -wm.top / kPxToMetric,
                                 wm.right / kPxToMetric, -wm.bottom / kPxToMetric);
  float v = ui::leptonica::LevelXToValue(band, pos.x / kPxToMetric, 0.f, vmax);
  if (auto t = widget->LockSel()) {
    {
      auto lock = std::lock_guard(t->mutex);
      switch (which) {
        case 0:
          t->lo = std::clamp((int)std::lround(v), 0, t->hi);
          break;
        case 1:
          t->hi = std::clamp((int)std::lround(v), t->lo, (int)vmax);
          break;
        case 2:
          t->lo2 = std::clamp((int)std::lround(v), 0, t->hi2);
          break;
        default:
          t->hi2 = std::clamp((int)std::lround(v), t->lo2, 255);
          break;
      }
    }
    t->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Select::MakeToy(ui::Widget* parent) {
  return std::make_unique<SelectToy>(parent, *this);
}

// ============================================================================
// Fade
// ============================================================================
Pix* Fade::ApplyOp(Pix* in, const float*) const {
  int d;
  bool blk;
  float rc, st;
  {
    auto lock = std::lock_guard(mutex);
    d = std::clamp(dir, 0, 3);
    blk = to_black;
    rc = std::clamp(reach, 0.02f, 1.f);
    st = std::clamp(strength, 0.f, 1.f);
  }
  static const int kDirs[] = {L_FROM_LEFT, L_FROM_RIGHT, L_FROM_TOP, L_FROM_BOT};
  Pix* out = pixCopy(nullptr, in);
  if (!out) return nullptr;
  if (pixLinearEdgeFade(out, kDirs[d], blk ? L_BLEND_TO_BLACK : L_BLEND_TO_WHITE, rc, st)) {
    pixDestroy(&out);
    return nullptr;
  }
  return out;
}
void Fade::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("dir");
  writer.Int(dir);
  writer.Key("to_black");
  writer.Bool(to_black);
  writer.Key("reach");
  writer.Double(reach);
  writer.Key("strength");
  writer.Double(strength);
}
bool Fade::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "dir") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) dir = std::clamp(v, 0, 3);
  } else if (key == "to_black") {
    bool v = false;
    d.Get(v, status);
    if (OK(status)) to_black = v;
  } else if (key == "reach") {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) reach = std::clamp((float)x, 0.f, 1.f);
  } else if (key == "strength") {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) strength = std::clamp((float)x, 0.f, 1.f);
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct FadeToy;
struct FadePoke : Action {
  FadePoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};
struct FadeSliderDrag : Action {
  TrackedPtr<FadeToy> widget;
  int which;  // 0 reach, 1 strength
  FadeSliderDrag(ui::Pointer& p, FadeToy& w, int which);
  void Update() override;
};

struct FadeToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int dir = 0;
  bool to_black = false;
  float reach = 0.4f;
  float strength = 0.9f;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kTop = 1.1_cm;
  constexpr static float kBottom = -5.00_cm;

  Ptr<Fade> LockFade() const { return LockObject<Fade>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  FadeToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockFade()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      dir = t->dir;
      to_black = t->to_black;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.7_cm, -1.0_cm, 2.7_cm, 0.9_cm); }
  SkPoint DirWheelCM() const { return {1.9_cm, -1.75_cm}; }
  constexpr static float kDirWheelR = 0.4_cm;
  Rect PolarityM() const { return Rect(-2.7_cm, -1.95_cm, -1.2_cm, -1.55_cm); }
  Rect ReachSliderM() const { return Rect(-2.7_cm, -2.85_cm, 0.6_cm, -2.5_cm); }
  Rect StrengthSliderM() const { return Rect(-2.7_cm, -3.5_cm, 0.6_cm, -3.15_cm); }

  SkPath Shape() const override {
    float base = 0.25_cm + 0.55_cm * std::clamp(reach, 0.f, 1.f);
    SkPathBuilder b;
    float l = -kHalfW, r = kHalfW, t = kTop, bo = kBottom;
    b.addRect(Rect(l, bo, r, t).sk);
    auto bar = [&](int i, float frac) {
      float w = base * frac;
      if (dir <= 1) {
        float y1 = t - 0.25_cm - i * 0.85_cm;
        float y0 = y1 - 0.5_cm;
        if (dir == 0)
          b.addRect(Rect(l - w, y0, l + 0.02_cm, y1).sk);
        else
          b.addRect(Rect(r - 0.02_cm, y0, r + w, y1).sk);
      } else {
        float x0 = -2.5_cm + i * 1.8_cm;
        float x1 = x0 + 1.1_cm;
        if (dir == 2)
          b.addRect(Rect(x0, t - 0.02_cm, x1, t + w).sk);
        else
          b.addRect(Rect(x0, bo - w, x1, bo + 0.02_cm).sk);
      }
    };
    bar(0, 1.f);
    bar(1, 0.62f);
    bar(2, 0.3f);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kTop).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 1.1_cm, kBottom - 1.1_cm, kHalfW + 1.1_cm, kTop + 1.1_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.2_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t FadeHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockFade()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)t->dir;
      h *= 16777619u;
      h ^= (uint32_t)(t->to_black ? 1 : 0);
      h *= 16777619u;
      h ^= (uint32_t)std::lround(t->reach * 512.f);
      h *= 16777619u;
      h ^= (uint32_t)std::lround(t->strength * 512.f);
      h *= 16777619u;
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockFade()) {
      uint32_t h = FadeHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        dir = t->dir;
        to_black = t->to_black;
        reach = t->reach;
        strength = t->strength;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xFA01, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0xFA90);

      {
        static const char* const kDirLabels[] = {"TOP", "RIGHT", "BOT", "LEFT"};
        static const int kDirAtPos[] = {2, 1, 3, 0};  // wheel position -> dir value
        int sel_pos = 0;
        for (int k = 0; k < 4; ++k)
          if (kDirAtPos[k] == dir) sel_pos = k;
        SkPoint wc = P(DirWheelCM().fX, DirWheelCM().fY);
        ui::leptonica::DrawModeWheel(canvas, wc, kDirWheelR / kPxToMetric, kDirLabels, 4, sel_pos,
                                     slop::State::Default, 0xFA10);
      }
      ui::leptonica::DrawPolarity(canvas, RPX(PolarityM()), !to_black, slop::State::Default,
                                  0xFA20);
      slop::DrawText(canvas, "FADE TO", P(PolarityM().left, PolarityM().top + 0.12_cm), 13.f,
                     kLabelInk, false, 0);

      {
        Rect rs = ReachSliderM();
        slop::Slider(canvas, RPX(rs), std::clamp(reach, 0.f, 1.f), slop::State::Default, 0xFA30);
        char buf[20];
        snprintf(buf, sizeof(buf), "REACH %.2f", reach);
        slop::DrawText(canvas, buf, P(rs.left, rs.top + 0.12_cm), 14.f, kLabelInk, false, 0);
      }
      {
        Rect ss = StrengthSliderM();
        slop::Slider(canvas, RPX(ss), std::clamp(strength, 0.f, 1.f), slop::State::Default, 0xFA31);
        char buf[24];
        snprintf(buf, sizeof(buf), "STRENGTH %.2f", strength);
        slop::DrawText(canvas, buf, P(ss.left, ss.top + 0.12_cm), 14.f, kLabelInk, false, 0);
      }

      std::string title = "FADE";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -3.85_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xFA40);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -4.12_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0xFA41);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint ppx{pos.x / kPxToMetric, -pos.y / kPxToMetric};
      SkPoint wc = DirWheelCM();
      int hit = ui::leptonica::ModeWheelHit({wc.fX / kPxToMetric, -wc.fY / kPxToMetric},
                                            kDirWheelR / kPxToMetric, ppx, 4);
      if (hit >= 0) {
        static const int kDirAtPos[] = {2, 1, 3, 0};
        if (auto t = LockFade()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->dir = kDirAtPos[hit];
          }
          t->WakeToys();
        }
        return std::make_unique<FadePoke>(p);
      }
      Rect pm = PolarityM();
      if (pos.x >= pm.left && pos.x <= pm.right && pos.y >= pm.bottom - 0.1_cm &&
          pos.y <= pm.top + 0.1_cm) {
        if (auto t = LockFade()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->to_black = !t->to_black;
          }
          t->WakeToys();
        }
        return std::make_unique<FadePoke>(p);
      }
      auto inRect = [&](const Rect& r) {
        return pos.x >= r.left - 0.2_cm && pos.x <= r.right + 0.2_cm &&
               pos.y >= r.bottom - 0.15_cm && pos.y <= r.top + 0.15_cm;
      };
      if (inRect(ReachSliderM())) return std::make_unique<FadeSliderDrag>(p, *this, 0);
      if (inRect(StrengthSliderM())) return std::make_unique<FadeSliderDrag>(p, *this, 1);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

FadeSliderDrag::FadeSliderDrag(ui::Pointer& p, FadeToy& w, int which)
    : Action(p), widget(&w), which(which) {}
void FadeSliderDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = which == 0 ? widget->ReachSliderM() : widget->StrengthSliderM();
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  if (auto f = widget->LockFade()) {
    {
      auto lock = std::lock_guard(f->mutex);
      if (which == 0)
        f->reach = t;
      else
        f->strength = t;
    }
    f->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Fade::MakeToy(ui::Widget* parent) {
  return std::make_unique<FadeToy>(parent, *this);
}

// ============================================================================
// Reduce
// ============================================================================
Pix* Reduce::ApplyOp(Pix* in, const float*) const {
  int fi, ru, rk;
  {
    auto lock = std::lock_guard(mutex);
    fi = std::clamp(factor_idx, 0, 3);
    ru = std::clamp(rule, 0, 4);
    rk = std::clamp(rank, 1, 4);
  }
  static const int kFactors[] = {2, 3, 4, 8};
  int f = kFactors[fi];
  if (ru == 0) {
    // GRAY: binarize, then the antialiased integer reduction (the classic scan shrink).
    Pix* b = pixConvertTo1(in, 128);
    if (!b) return nullptr;
    Pix* out = f == 2   ? pixScaleToGray2(b)
               : f == 3 ? pixScaleToGray3(b)
               : f == 4 ? pixScaleToGray4(b)
                        : pixScaleToGray8(b);
    pixDestroy(&b);
    return out;
  }
  Pix* g = pixConvertTo8(in, 0);
  if (!g) return nullptr;
  if (ru == 4) {
    // RANK: cascade x2 rank reductions (x3 has no rank form - falls back to one stage).
    int stages = f == 8 ? 3 : f == 4 ? 2 : 1;
    Pix* cur = g;
    for (int s = 0; s < stages && cur; ++s) {
      Pix* nxt = pixScaleGrayRank2(cur, rk);
      pixDestroy(&cur);
      cur = nxt;
    }
    return cur;
  }
  int type = ru == 1 ? L_CHOOSE_MIN : ru == 2 ? L_CHOOSE_MAX : L_CHOOSE_MAXDIFF;
  if (f == 8) {
    // MIN/MAX/DIFF take factors up to 4 - cascade 4x then 2x for x8.
    Pix* mid = pixScaleGrayMinMax(g, 4, 4, type);
    pixDestroy(&g);
    if (!mid) return nullptr;
    Pix* out = pixScaleGrayMinMax(mid, 2, 2, type);
    pixDestroy(&mid);
    return out;
  }
  Pix* out = pixScaleGrayMinMax(g, f, f, type);
  pixDestroy(&g);
  return out;
}
void Reduce::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("factor_idx");
  writer.Int(factor_idx);
  writer.Key("rule");
  writer.Int(rule);
  writer.Key("rank");
  writer.Int(rank);
}
bool Reduce::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "factor_idx") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) factor_idx = std::clamp(v, 0, 3);
  } else if (key == "rule") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) rule = std::clamp(v, 0, 4);
  } else if (key == "rank") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) rank = std::clamp(v, 1, 4);
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct ReduceToy;
struct ReducePoke : Action {
  ReducePoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};
struct ReduceRankDrag : Action {
  TrackedPtr<ReduceToy> widget;
  ReduceRankDrag(ui::Pointer& p, ReduceToy& w);
  void Update() override;
};

struct ReduceToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int factor_idx = 0;
  int rule = 0;
  int rank = 2;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kShelf = 0.95_cm;  // body top; the steps rise from here
  constexpr static float kTop = 1.95_cm;    // tallest step
  constexpr static float kBottom = -4.35_cm;

  Ptr<Reduce> LockRed() const { return LockObject<Reduce>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  ReduceToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockRed()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      factor_idx = t->factor_idx;
      rule = t->rule;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.7_cm, -1.0_cm, 2.7_cm, 0.85_cm); }
  Rect FactorChipM(int i) const {
    float x0 = -2.7_cm + i * 0.62_cm;
    return Rect(x0, -1.65_cm, x0 + 0.52_cm, -1.3_cm);
  }
  SkPoint RuleWheelCM() const { return {1.9_cm, -1.7_cm}; }
  constexpr static float kRuleWheelR = 0.42_cm;
  Rect RankRowM() const { return Rect(-2.7_cm, -2.55_cm, 0.6_cm, -2.2_cm); }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kShelf).sk);
    b.addRect(Rect(-kHalfW, kShelf - 0.1_cm, -1.0_cm, kTop).sk);
    b.addRect(Rect(-1.0_cm, kShelf - 0.1_cm, 1.1_cm, kShelf + 0.55_cm).sk);
    b.addRect(Rect(1.1_cm, kShelf - 0.1_cm, kHalfW, kShelf + 0.22_cm).sk);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kTop).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.5_cm, kHalfW + 0.5_cm, kTop + 0.5_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.2_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t RedHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockRed()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)t->factor_idx;
      h *= 16777619u;
      h ^= (uint32_t)t->rule;
      h *= 16777619u;
      h ^= (uint32_t)t->rank;
      h *= 16777619u;
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockRed()) {
      uint32_t h = RedHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        factor_idx = t->factor_idx;
        rule = t->rule;
        rank = t->rank;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xDE01, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0xDE90);

      static const char* const kFactors[] = {
          "\xc3\x97"
          "2",
          "\xc3\x97"
          "3",
          "\xc3\x97"
          "4",
          "\xc3\x97"
          "8"};
      for (int i = 0; i < 4; ++i) {
        SkRect chip = RPX(FactorChipM(i));
        bool sel = i == factor_idx;
        bool na = rule == 4 && i == 1;  // RANK has no x3 form
        SkPath cp = slop::WonkyRoundRect(chip, chip.height() * 0.3f, slop::kWonk * 0.5f,
                                         0xDE20u + (uint32_t)i);
        if (sel) slop::HandShadow(canvas, cp, {2.f, 2.f}, slop::kShadow, 0xDE24u + (uint32_t)i);
        slop::MisregFill(canvas, cp, na ? slop::kGray : (sel ? slop::kPaper : slop::kGray),
                         0xDE28u + (uint32_t)i);
        slop::SketchyStroke(canvas, cp, na ? slop::kGray : slop::kInk,
                            sel ? slop::kStroke : slop::kStrokeHair, 0xDE2Cu + (uint32_t)i, 1);
        float lw = slop::TextWidth(kFactors[i], 13.f);
        slop::DrawText(canvas, kFactors[i], {chip.centerX() - lw / 2, chip.centerY() + 4.5f}, 13.f,
                       na ? slop::kGrayDark : (sel ? slop::kInk : slop::kInkSoft), false, 0);
        if (sel) slop::Highlight(canvas, chip, slop::kBlue, 0xDE34);
        if (na)
          slop::HatchRect(canvas, chip, slop::kInkSoft, chip.height() * 0.3f,
                          0xDE38u + (uint32_t)i);
      }
      slop::DrawText(canvas, "FACTOR", P(FactorChipM(0).left, FactorChipM(0).top + 0.12_cm), 13.f,
                     kLabelInk, false, 0);

      {
        static const char* const kRules[] = {"GRAY", "MIN", "MAX", "DIFF", "RANK"};
        SkPoint wc = P(RuleWheelCM().fX, RuleWheelCM().fY);
        ui::leptonica::DrawModeWheel(canvas, wc, kRuleWheelR / kPxToMetric, kRules, 5, rule,
                                     slop::State::Default, 0xDE40);
      }

      if (rule == 4) {
        Rect rr = RankRowM();
        slop::Slider(canvas, RPX(rr), (rank - 1) / 3.f, slop::State::Default, 0xDE50);
        char buf[16];
        snprintf(buf, sizeof(buf), "RANK %d", rank);
        slop::DrawText(canvas, buf, P(rr.left, rr.top + 0.12_cm), 14.f, kLabelInk, false, 0);
      }

      std::string title = "REDUCE";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(-0.5_cm, -3.2_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xDE60);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(-0.5_cm, -3.47_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0xDE61);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint ppx{pos.x / kPxToMetric, -pos.y / kPxToMetric};
      for (int i = 0; i < 4; ++i) {
        Rect r = FactorChipM(i);
        if (pos.x >= r.left && pos.x <= r.right && pos.y >= r.bottom && pos.y <= r.top) {
          if (rule == 4 && i == 1) return std::make_unique<ReducePoke>(p);  // x3 N/A under RANK
          if (auto t = LockRed()) {
            {
              auto lock = std::lock_guard(t->mutex);
              t->factor_idx = i;
            }
            t->WakeToys();
          }
          return std::make_unique<ReducePoke>(p);
        }
      }
      SkPoint wc = RuleWheelCM();
      int hit = ui::leptonica::ModeWheelHit({wc.fX / kPxToMetric, -wc.fY / kPxToMetric},
                                            kRuleWheelR / kPxToMetric, ppx, 5);
      if (hit >= 0) {
        if (auto t = LockRed()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->rule = hit;
            if (hit == 4 && t->factor_idx == 1) t->factor_idx = 0;  // x3 has no rank form
          }
          t->WakeToys();
        }
        return std::make_unique<ReducePoke>(p);
      }
      if (rule == 4) {
        Rect rr = RankRowM();
        if (pos.x >= rr.left - 0.2_cm && pos.x <= rr.right + 0.2_cm &&
            pos.y >= rr.bottom - 0.15_cm && pos.y <= rr.top + 0.15_cm)
          return std::make_unique<ReduceRankDrag>(p, *this);
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

ReduceRankDrag::ReduceRankDrag(ui::Pointer& p, ReduceToy& w) : Action(p), widget(&w) {}
void ReduceRankDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = widget->RankRowM();
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  if (auto q = widget->LockRed()) {
    {
      auto lock = std::lock_guard(q->mutex);
      q->rank = 1 + (int)std::lround(t * 3.f);
    }
    q->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Reduce::MakeToy(ui::Widget* parent) {
  return std::make_unique<ReduceToy>(parent, *this);
}

// ============================================================================
// Measure
// ============================================================================
// Gray the image, clip to the region, read MEAN (pixAverageInRect) or
// MIN/MAX (pixGetExtremeValue) off the clip.
static bool MeasureRegionValue(Pix* pix32, float u0, float v0, float u1, float v1, int stat,
                               int& out) {
  Pix* gray = pixConvertRGBToLuminance(pix32);
  if (!gray) return false;
  int w = pixGetWidth(gray), h = pixGetHeight(gray);
  int x = std::clamp((int)std::lround(u0 * w), 0, w - 1);
  int y = std::clamp((int)std::lround(v0 * h), 0, h - 1);
  int bw = std::clamp((int)std::lround((u1 - u0) * w), 1, w - x);
  int bh = std::clamp((int)std::lround((v1 - v0) * h), 1, h - y);
  bool ok = false;
  if (stat == 0) {
    BOX* box = boxCreate(x, y, bw, bh);
    l_float32 ave = 0;
    if (box && pixAverageInRect(gray, nullptr, box, 0, 255, 1, &ave) == 0) {
      out = (int)std::lround(ave);
      ok = true;
    }
    if (box) boxDestroy(&box);
  } else {
    BOX* box = boxCreate(x, y, bw, bh);
    Pix* clip = box ? pixClipRectangle(gray, box, nullptr) : nullptr;
    if (box) boxDestroy(&box);
    if (clip) {
      l_int32 gv = 0;
      if (pixGetExtremeValue(clip, 1, stat == 1 ? L_SELECT_MIN : L_SELECT_MAX, nullptr, nullptr,
                             nullptr, &gv) == 0) {
        out = gv;
        ok = true;
      }
      pixDestroy(&clip);
    }
  }
  pixDestroy(&gray);
  return ok;
}

void Measure::PushValue(int v) {
  if (auto target = value->ObjectOrNull()) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", v);
    target->SetText(buf);
  }
}

void Measure::DoMeasure() {
  auto nested = image->FindInterface();
  Object* owner = nested.Owner<Object>();
  ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
  sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
  if (!source) return;
  Pix* pix = SkImageToPix(source);
  if (!pix) return;
  float a0, b0, a1, b1;
  int st;
  {
    auto lock = std::lock_guard(mutex);
    a0 = u0;
    b0 = v0;
    a1 = u1;
    b1 = v1;
    st = stat;
  }
  int v = 0;
  bool ok = MeasureRegionValue(pix, a0, b0, a1, b1, st, v);
  pixDestroy(&pix);
  if (!ok) return;
  {
    auto lock = std::lock_guard(mutex);
    last_value = v;
  }
  PushValue(v);
  WakeToys();
}

void Measure::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("u0");
  writer.Double(u0);
  writer.Key("v0");
  writer.Double(v0);
  writer.Key("u1");
  writer.Double(u1);
  writer.Key("v1");
  writer.Double(v1);
  writer.Key("stat");
  writer.Int(stat);
}
bool Measure::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  auto get01 = [&](float& dst) {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) dst = std::clamp((float)x, 0.f, 1.f);
  };
  if (key == "u0") {
    get01(u0);
  } else if (key == "v0") {
    get01(v0);
  } else if (key == "u1") {
    get01(u1);
  } else if (key == "v1") {
    get01(v1);
  } else if (key == "stat") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) stat = std::clamp(v, 0, 2);
    return true;
  } else {
    return false;
  }
  return true;
}

struct MeasureToy;
struct MeasureRegionDrag : Action {
  TrackedPtr<MeasureToy> widget;
  int which;
  float grab_du = 0.f, grab_dv = 0.f;
  MeasureRegionDrag(ui::Pointer& p, MeasureToy& w, int which);
  ~MeasureRegionDrag();
  void Update() override;
};
struct MeasurePoke : Action {
  MeasurePoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct MeasureToy : ObjectToy {
  sk_sp<SkImage> cached_preview;  // the SOURCE (the marquee rides on it)
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  bool has_measure = false;
  int found_value = 0;

  float u0 = 0.25f, v0 = 0.25f, u1 = 0.75f, v1 = 0.75f;
  int stat = 0;
  int dragging = 0;
  uintptr_t last_push_target = 0;
  int last_pushed = -1;

  std::unique_ptr<ui::slop::RunButton> glass;


  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;
  constexpr static float kBottom = -4.00_cm;

  Ptr<Measure> LockMeasure() const { return LockObject<Measure>(); }

  MeasureToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockMeasure()) t->measure->ScheduleRun();
    });
    if (auto t = LockMeasure()) {
      auto lock = std::lock_guard(t->mutex);
      u0 = t->u0;
      v0 = t->v0;
      u1 = t->u1;
      v1 = t->v1;
      stat = t->stat;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.85_cm, -1.3_cm, 2.85_cm, 0.7_cm); }
  Rect StatCellM(int which) const {
    float left = -2.7_cm + which * 0.95_cm;
    return Rect(left, -2.25_cm, left + 0.85_cm, -1.75_cm);
  }
  // Reaches down past kBodyTop so the chip is one contour with the body (offset outlines).
  Rect ValueChipM() const { return Rect(0.65_cm, 0.88_cm, 2.45_cm, 2.0_cm); }

  Rect FittedRectM() const {
    Rect area = PreviewM();
    if (!cached_preview) return area;
    float iw = (float)cached_preview->width(), ih = (float)cached_preview->height();
    if (iw <= 0 || ih <= 0) return area;
    float s = std::min(area.Width() / iw, area.Height() / ih);
    return Rect::MakeCenter({area.CenterX(), area.CenterY()}, iw * s, ih * s);
  }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    SkPathBuilder fan;
    fan.moveTo(-2.3_cm, 0.9_cm);
    for (int i = 0; i <= 12; ++i) {
      float a = 3.14159265f * 0.5f + 3.14159265f * 0.5f * (float)i / 12.f;  // 90..180 deg
      fan.lineTo(-2.3_cm + 1.3_cm * std::cos(a), 0.9_cm + 1.3_cm * std::sin(a));
    }
    fan.close();
    b.addPath(fan.detach());
    b.addRect(ValueChipM().sk);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, 2.2_cm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, 2.6_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&Measure::image_tbl))
      return {.pos = {-kHalfW, 0.5_cm}, .dir = 180_deg};
    if (&arg == static_cast<const Interface::Table*>(&Measure::value_tbl))
      return {.pos = {-kHalfW, -0.5_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t MeasureHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockMeasure()) {
      auto lock = std::lock_guard(t->mutex);
      for (float f : {t->u0, t->v0, t->u1, t->v1}) {
        h ^= (uint32_t)std::lround(f * 4096.f);
        h *= 16777619u;
      }
      h ^= (uint32_t)t->stat;
      h *= 16777619u;
    }
    return h;
  }

  void RecomputeMeasure() {
    auto tool = LockMeasure();
    cached_preview = nullptr;
    has_measure = false;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(1.f, std::min(300.f / sw, 300.f / sh));
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    cached_preview = surf->makeImageSnapshot();
    if (!cached_preview) return;
    Pix* pix = SkImageToPix(cached_preview);
    if (!pix) return;
    float a0, b0, a1, b1;
    int st;
    {
      auto lock = std::lock_guard(tool->mutex);
      a0 = std::clamp(tool->u0, 0.f, 1.f);
      b0 = std::clamp(tool->v0, 0.f, 1.f);
      a1 = std::clamp(tool->u1, 0.f, 1.f);
      b1 = std::clamp(tool->v1, 0.f, 1.f);
      st = tool->stat;
    }
    int v = 0;
    bool ok = MeasureRegionValue(pix, a0, b0, a1, b1, st, v);
    pixDestroy(&pix);
    if (ok) {
      found_value = v;
      has_measure = true;
      auto lock = std::lock_guard(tool->mutex);
      tool->last_value = v;
    }
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    if (auto t = LockMeasure()) {
      uint32_t h = MeasureHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      uint32_t id = 0;
      {
        auto nested = t->image->FindInterface();
        Object* owner = nested.Owner<Object>();
        ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
        sk_sp<SkImage> img = ip ? ip.GetImage() : nullptr;
        id = SourceImageId(img);
      }
      if (id != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputeMeasure();
      {
        auto lock = std::lock_guard(t->mutex);
        u0 = t->u0;
        v0 = t->v0;
        u1 = t->u1;
        v1 = t->v1;
        stat = t->stat;
      }
      if (has_measure) {
        uintptr_t tgt = 0;
        if (auto target = t->value->ObjectOrNull()) tgt = (uintptr_t)&*target;
        if (tgt != 0 && (tgt != last_push_target || found_value != last_pushed)) {
          t->PushValue(found_value);
          last_push_target = tgt;
          last_pushed = found_value;
        }
      }
    }
    return animation::Animating;  // finder: keep ticking while unconnected
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    {
      float frac = has_measure ? std::clamp(found_value / 255.f, 0.f, 1.f) : 0.5f;
      float a = 3.14159265f - 3.14159265f * 0.5f * frac;  // 180..90 deg
      SkPaint np;
      np.setAntiAlias(true);
      np.setStyle(SkPaint::kStroke_Style);
      np.setStrokeWidth(0.06_cm);
      np.setColor("#ed1c24"_color);
      canvas.drawLine(-2.3_cm, 0.9_cm, -2.3_cm + 1.15_cm * std::cos(a),
                      0.9_cm + 1.15_cm * std::sin(a), np);
    }

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xE301, 1);

      if (cached_preview) {
        SkRect fit = RPX(FittedRectM());
        SkRect mq = SkRect::MakeLTRB(fit.fLeft + u0 * fit.width(), fit.fTop + v0 * fit.height(),
                                     fit.fLeft + u1 * fit.width(), fit.fTop + v1 * fit.height());
        ui::leptonica::DrawRegion(canvas, fit, mq,
                                  dragging ? slop::State::Pressed : slop::State::Default, 0xE310);
      }

      {
        SkRect chip = RPX(ValueChipM());
        slop::MisregFill(canvas, slop::WonkyRoundRect(chip, 8.f, slop::kWonk, 0xE320), slop::kPaper,
                         0xE321);
        slop::SketchyStroke(canvas, slop::WonkyRoundRect(chip, 8.f, slop::kWonk, 0xE320),
                            slop::kInk, slop::kStroke, 0xE322, 2);
        char buf[16];
        if (has_measure)
          snprintf(buf, sizeof(buf), "%d", found_value);
        else
          snprintf(buf, sizeof(buf), "-");
        float fs = 36.f;
        float tw = slop::TextWidth(buf, fs);
        slop::DrawText(canvas, buf, {chip.centerX() - tw / 2, chip.centerY() + fs * 0.36f}, fs,
                       slop::kInk, false, 0);
      }

      {
        static const char* const kStat[3] = {"MEAN", "MIN", "MAX"};
        for (int which = 0; which < 3; ++which) {
          SkRect cell = RPX(StatCellM(which));
          bool sel = stat == which;
          SkPath cp = slop::WonkyRoundRect(cell, cell.height() * 0.3f, slop::kWonk * 0.5f,
                                           0xE330u + (uint32_t)which);
          if (sel)
            slop::HandShadow(canvas, cp, {2.f, 2.f}, slop::kShadow, 0xE334u + (uint32_t)which);
          slop::MisregFill(canvas, cp, sel ? slop::kPaper : slop::kGray, 0xE338u + (uint32_t)which);
          slop::SketchyStroke(canvas, cp, slop::kInk, sel ? slop::kStroke : slop::kStrokeHair,
                              0xE33Cu + (uint32_t)which, 1);
          float lw = slop::TextWidth(kStat[which], 12.f);
          slop::DrawText(canvas, kStat[which], {cell.centerX() - lw / 2, cell.centerY() + 4.f},
                         12.f, sel ? slop::kInk : slop::kInkSoft, false, 0);
          if (sel) slop::Highlight(canvas, cell, slop::kBlue, 0xE340);
        }
      }

      std::string title = "MEASURE";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -2.85_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xE350);
      std::string credit = stat == 0 ? "pixAverageInRect()" : "pixGetExtremeValue()";
      float fw = slop::TextWidth(credit, kCreditTextPx);
      SkPoint fc = P(0, -3.13_cm);
      slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                     false, 0xE351);
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      for (int which = 0; which < 3; ++which) {
        Rect r = StatCellM(which);
        if (pos.x >= r.left && pos.x <= r.right && pos.y >= r.bottom && pos.y <= r.top) {
          if (auto t = LockMeasure()) {
            {
              auto lock = std::lock_guard(t->mutex);
              t->stat = which;
            }
            t->WakeToys();
          }
          return std::make_unique<MeasurePoke>(p);
        }
      }
      if (cached_preview) {
        SkPoint ppx{pos.x / kPxToMetric, -pos.y / kPxToMetric};
        Rect fm = FittedRectM();
        SkRect fit = SkRect::MakeLTRB(fm.left / kPxToMetric, -fm.top / kPxToMetric,
                                      fm.right / kPxToMetric, -fm.bottom / kPxToMetric);
        SkRect mq = SkRect::MakeLTRB(fit.fLeft + u0 * fit.width(), fit.fTop + v0 * fit.height(),
                                     fit.fLeft + u1 * fit.width(), fit.fTop + v1 * fit.height());
        int hit = ui::leptonica::RegionHit(mq, ppx);
        if (hit != 0) return std::make_unique<MeasureRegionDrag>(p, *this, hit);
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

MeasureRegionDrag::MeasureRegionDrag(ui::Pointer& p, MeasureToy& w, int which)
    : Action(p), widget(&w), which(which) {
  widget->dragging = which;
  if (which == 5) {
    Vec2 pos = p.PositionWithin(w);
    Rect fit = w.FittedRectM();
    if (fit.Width() > 0 && fit.Height() > 0) {
      grab_du = (pos.x - fit.left) / fit.Width() - w.u0;
      grab_dv = (fit.top - pos.y) / fit.Height() - w.v0;
    }
  }
}
MeasureRegionDrag::~MeasureRegionDrag() {
  if (widget) {
    widget->dragging = 0;
    widget->WakeAnimation();
  }
}
void MeasureRegionDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect fit = widget->FittedRectM();
  if (fit.Width() <= 0 || fit.Height() <= 0) return;
  float u = std::clamp((pos.x - fit.left) / fit.Width(), 0.f, 1.f);
  float v = std::clamp((fit.top - pos.y) / fit.Height(), 0.f, 1.f);
  constexpr float kMin = 0.02f;
  if (auto t = widget->LockMeasure()) {
    {
      auto lock = std::lock_guard(t->mutex);
      switch (which) {
        case 1:
          t->u0 = std::min(u, t->u1 - kMin);
          t->v0 = std::min(v, t->v1 - kMin);
          break;
        case 2:
          t->u1 = std::max(u, t->u0 + kMin);
          t->v0 = std::min(v, t->v1 - kMin);
          break;
        case 3:
          t->u0 = std::min(u, t->u1 - kMin);
          t->v1 = std::max(v, t->v0 + kMin);
          break;
        case 4:
          t->u1 = std::max(u, t->u0 + kMin);
          t->v1 = std::max(v, t->v0 + kMin);
          break;
        case 5: {
          float w0 = t->u1 - t->u0, h0 = t->v1 - t->v0;
          float nu0 = std::clamp(u - grab_du, 0.f, 1.f - w0);
          float nv0 = std::clamp(v - grab_dv, 0.f, 1.f - h0);
          t->u0 = nu0;
          t->v0 = nv0;
          t->u1 = nu0 + w0;
          t->v1 = nv0 + h0;
          break;
        }
      }
    }
    t->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Measure::MakeToy(ui::Widget* parent) {
  return std::make_unique<MeasureToy>(parent, *this);
}

// ============================================================================
// Warp
// ============================================================================
Pix* Warp::ApplyOp(Pix* in, const float*) const {
  int m;
  float amt;
  {
    auto lock = std::lock_guard(mutex);
    m = mode;
    amt = std::clamp(amount, 0.f, 1.f);
  }
  if (m == 0) {
    return pixStretchHorizontal(in, L_WARP_TO_RIGHT, L_QUADRATIC_WARP,
                                std::max(1, (int)std::lround(amt * 100.f)), L_INTERPOLATED,
                                L_BRING_IN_WHITE);
  }
  if (m == 1) {
    int v = std::max(1, (int)std::lround(amt * 100.f));
    return pixQuadraticVShear(in, L_WARP_TO_RIGHT, v, v, L_INTERPOLATED, L_BRING_IN_WHITE);
  }
  Pix* g = pixConvertTo8(in, 0);
  if (!g) return nullptr;
  float mag = amt * 15.f;
  Pix* out = pixRandomHarmonicWarp(g, mag, mag, 0.12f, 0.12f, 2, 2, 7, 128);
  pixDestroy(&g);
  return out;  // 8 bpp gray in WAVES mode - preview + surface upconvert
}
void Warp::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("mode");
  writer.Int(mode);
  writer.Key("amount");
  writer.Double(amount);
}
bool Warp::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "mode") {
    int v = 0;
    d.Get(v, status);
    if (OK(status)) mode = std::clamp(v, 0, 1);
  } else if (key == "amount") {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) amount = std::clamp((float)x, 0.f, 1.f);
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct WarpToy;
struct WarpAmountDrag : Action {
  TrackedPtr<WarpToy> widget;
  WarpAmountDrag(ui::Pointer& p, WarpToy& w);
  ~WarpAmountDrag();
  void Update() override;
};
struct WarpPoke : Action {
  WarpPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct WarpToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int mode = 2;
  float amount = 0.5f;
  bool dragging = false;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;
  constexpr static float kBottom = -4.00_cm;

  Ptr<Warp> LockWarp() const { return LockObject<Warp>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  WarpToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockWarp()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      mode = t->mode;
      amount = t->amount;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.85_cm, -1.1_cm, 2.85_cm, 0.75_cm); }
  Rect AmountSliderM() const { return Rect(-2.7_cm, -1.95_cm, 0.0_cm, -1.6_cm); }
  SkPoint ModeWheelCM() const { return {1.0_cm, -2.0_cm}; }
  constexpr static float kModeWheelR = 0.55_cm;

  SkPath Shape() const override {
    float amp = 0.12_cm + std::clamp(amount, 0.f, 1.f) * 0.55_cm;
    SkPathBuilder b;
    SkPathBuilder body;
    body.moveTo(-kHalfW, kBottom);
    body.lineTo(kHalfW, kBottom);
    body.lineTo(kHalfW, kBodyTop);
    for (int i = 0; i <= 40; ++i) {
      float x = kHalfW - 2.f * kHalfW * (float)i / 40.f;
      float y = kBodyTop + amp * (0.5f + 0.5f * std::sin((float)i / 40.f * 12.566f));
      body.lineTo(x, y);
    }
    body.close();
    b.addPath(body.detach());
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop + 0.7_cm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, kBodyTop + 1.2_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.0_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t WarpHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockWarp()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)t->mode;
      h *= 16777619u;
      h ^= (uint32_t)std::lround(t->amount * 1024.f);
      h *= 16777619u;
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockWarp()) {
      uint32_t h = WarpHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        mode = t->mode;
        amount = t->amount;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xA201, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0xA290);

      Rect as = AmountSliderM();
      slop::Slider(canvas, RPX(as), std::clamp(amount, 0.f, 1.f), slop::State::Default, 0xA210);
      {
        char buf[20];
        snprintf(buf, sizeof(buf), "AMOUNT %.2f", amount);
        slop::DrawText(canvas, buf, P(as.left, as.top + 0.12_cm), 14.f, kLabelInk, false, 0);
      }

      {
        static const char* const kModes[3] = {"STRETCH", "SHEAR", "WAVES"};
        SkPoint wc = P(ModeWheelCM().fX, ModeWheelCM().fY);
        ui::leptonica::DrawModeWheel(canvas, wc, kModeWheelR / kPxToMetric, kModes, 3, mode,
                                     slop::State::Default, 0xA220);
      }

      std::string title = "WARP";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -2.85_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xA230);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -3.13_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0xA231);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint pp = {pos.x / kPxToMetric, -pos.y / kPxToMetric};
      SkPoint wc = {ModeWheelCM().fX / kPxToMetric, -ModeWheelCM().fY / kPxToMetric};
      int m = ui::leptonica::ModeWheelHit(wc, kModeWheelR / kPxToMetric, pp, 3);
      if (m >= 0) {
        if (auto t = LockWarp()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->mode = m;
          }
          t->WakeToys();
        }
        return std::make_unique<WarpPoke>(p);
      }
      Rect as = AmountSliderM();
      if (pos.x >= as.left - 0.2_cm && pos.x <= as.right + 0.2_cm && pos.y >= as.bottom - 0.2_cm &&
          pos.y <= as.top + 0.2_cm)
        return std::make_unique<WarpAmountDrag>(p, *this);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

WarpAmountDrag::WarpAmountDrag(ui::Pointer& p, WarpToy& w) : Action(p), widget(&w) {
  widget->dragging = true;
}
WarpAmountDrag::~WarpAmountDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void WarpAmountDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = widget->AmountSliderM();
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  if (auto w = widget->LockWarp()) {
    {
      auto lock = std::lock_guard(w->mutex);
      w->amount = t;
    }
    w->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Warp::MakeToy(ui::Widget* parent) {
  return std::make_unique<WarpToy>(parent, *this);
}

// ============================================================================
// Color
// ============================================================================
// The three colour adjustments chain, each skipped at neutral.
Pix* Color::ApplyOp(Pix* in, const float*) const {
  float h, s, rs, gs, bs;
  {
    auto lock = std::lock_guard(mutex);
    h = std::clamp(hue, -1.f, 1.f);
    s = std::clamp(sat, -1.f, 1.f);
    rs = std::clamp(r_shift, -1.f, 1.f);
    gs = std::clamp(g_shift, -1.f, 1.f);
    bs = std::clamp(b_shift, -1.f, 1.f);
  }
  Pix* cur = pixCopy(nullptr, in);
  if (!cur) return nullptr;
  if (h != 0.f) {
    Pix* o = pixModifyHue(nullptr, cur, h);
    pixDestroy(&cur);
    cur = o;
    if (!cur) return nullptr;
  }
  if (s != 0.f) {
    Pix* o = pixModifySaturation(nullptr, cur, s);
    pixDestroy(&cur);
    cur = o;
    if (!cur) return nullptr;
  }
  if (rs != 0.f || gs != 0.f || bs != 0.f) {
    Pix* o = pixColorShiftRGB(cur, rs, gs, bs);
    pixDestroy(&cur);
    cur = o;
  }
  return cur;
}
void Color::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("hue");
  writer.Double(hue);
  writer.Key("sat");
  writer.Double(sat);
  writer.Key("r_shift");
  writer.Double(r_shift);
  writer.Key("g_shift");
  writer.Double(g_shift);
  writer.Key("b_shift");
  writer.Double(b_shift);
}
bool Color::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  auto get1 = [&](float& dst) {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) dst = std::clamp((float)x, -1.f, 1.f);
  };
  if (key == "hue") {
    get1(hue);
  } else if (key == "sat") {
    get1(sat);
  } else if (key == "r_shift") {
    get1(r_shift);
  } else if (key == "g_shift") {
    get1(g_shift);
  } else if (key == "b_shift") {
    get1(b_shift);
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct ColorToy;
struct ColorHueDrag : Action {
  TrackedPtr<ColorToy> widget;
  ColorHueDrag(ui::Pointer& p, ColorToy& w);
  ~ColorHueDrag();
  void Update() override;
};
struct ColorSliderDrag : Action {
  TrackedPtr<ColorToy> widget;
  int which;  // 0 sat, 1 r, 2 g, 3 b
  ColorSliderDrag(ui::Pointer& p, ColorToy& w, int which);
  ~ColorSliderDrag();
  void Update() override;
};

struct ColorToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  float hue = 0.f, sat = 0.f, r_shift = 0.f, g_shift = 0.f, b_shift = 0.f;
  int dragging = -1;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;
  constexpr static float kBottom = -4.15_cm;
  constexpr static float kWheelR = 1.5_cm;  // the hue half-disc on the top edge

  Ptr<Color> LockColor() const { return LockObject<Color>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  ColorToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockColor()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      hue = t->hue;
      sat = t->sat;
      r_shift = t->r_shift;
      g_shift = t->g_shift;
      b_shift = t->b_shift;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.85_cm, -1.1_cm, 2.85_cm, 0.8_cm); }
  Rect SliderM(int which) const {
    float top = -1.45_cm - which * 0.42_cm;
    return Rect(-2.05_cm, top - 0.26_cm, 0.85_cm, top);
  }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    SkPathBuilder dome;
    dome.moveTo(-kWheelR, kBodyTop - 0.05_cm);
    for (int i = 0; i <= 24; ++i) {
      float a = 3.14159265f - 3.14159265f * (float)i / 24.f;  // pi..0
      dome.lineTo(kWheelR * std::cos(a), kBodyTop - 0.05_cm + kWheelR * std::sin(a));
    }
    dome.close();
    b.addPath(dome.detach());
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop + kWheelR).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, kBodyTop + kWheelR + 0.5_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.0_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t ColorHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockColor()) {
      auto lock = std::lock_guard(t->mutex);
      for (float f : {t->hue, t->sat, t->r_shift, t->g_shift, t->b_shift}) {
        h ^= (uint32_t)std::lround(f * 1024.f);
        h *= 16777619u;
      }
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    float scl = std::min(200.f / sw, 200.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    Pix* result = tool->ApplyOp(pix, nullptr);
    pixDestroy(&pix);
    if (!result) return;
    out_depth = pixGetDepth(result);
    out_cmap = pixGetColormap(result) != nullptr;
    Pix* r32 = result;
    if (pixGetDepth(result) != 32) {
      r32 = pixConvertTo32(result);
      pixDestroy(&result);
      if (!r32) return;
    }
    cached_preview = PixToSkImage(r32);
    if (r32 != result) pixDestroy(&r32);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockColor()) {
      uint32_t h = ColorHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        hue = t->hue;
        sat = t->sat;
        r_shift = t->r_shift;
        g_shift = t->g_shift;
        b_shift = t->b_shift;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    {
      float cy = kBodyTop - 0.05_cm;
      for (int i = 0; i < 12; ++i) {
        float a0 = 3.14159265f * (float)i / 12.f;
        float a1 = 3.14159265f * (float)(i + 1) / 12.f;
        SkPathBuilder w;
        w.moveTo(0, cy);
        for (int k = 0; k <= 4; ++k) {
          float a = a0 + (a1 - a0) * (float)k / 4.f;
          w.lineTo((kWheelR - 0.12_cm) * std::cos(a), cy + (kWheelR - 0.12_cm) * std::sin(a));
        }
        w.close();
        float middeg = (a0 + a1) * 0.5f * 57.29578f;
        float h = std::fmod(360.f + (180.f - middeg) + hue * 180.f, 360.f);
        SkPaint wp;
        wp.setAntiAlias(true);
        float hsv[3] = {h, 0.85f, 0.95f};
        wp.setColor(SkHSVToColor(hsv));
        canvas.drawPath(w.detach(), wp);
      }
      SkPaint np;
      np.setAntiAlias(true);
      np.setStyle(SkPaint::kStroke_Style);
      np.setStrokeWidth(0.08_cm);
      np.setColor("#1a1a1a"_color);
      float ang = 1.5707963f;  // up; the RAINBOW rotates, the needle stays - the wheel turns
      canvas.drawLine(0, cy, kWheelR * 0.92f * std::cos(ang), cy + kWheelR * 0.92f * std::sin(ang),
                      np);
    }

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xC101, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0xC190);

      static const char* const kLab[4] = {"SAT", "R", "G", "B"};
      static const SkColor kLabCol[4] = {0xff7a6f5c, 0xffed1c24, 0xff22b14c, 0xff3f48cc};
      const float vals[4] = {sat, r_shift, g_shift, b_shift};
      for (int i = 0; i < 4; ++i) {
        Rect s = SliderM(i);
        slop::Slider(canvas, RPX(s), std::clamp((vals[i] + 1.f) * 0.5f, 0.f, 1.f),
                     slop::State::Default, 0xC110u + (uint32_t)i);
        char buf[20];
        snprintf(buf, sizeof(buf), "%s %+.2f", kLab[i], vals[i]);
        slop::DrawText(canvas, buf, P(s.left - 0.62_cm, s.top - 0.02_cm), 12.f, kLabCol[i], false,
                       0);
        float cx = (RPX(s).fLeft + RPX(s).fRight) * 0.5f;
        canvas.drawPath(slop::WobbleLine({cx, RPX(s).fTop - 3.f}, {cx, RPX(s).fTop + 5.f}, 0.8f,
                                         5.f, 0xC120u + (uint32_t)i),
                        slop::InkPaint(slop::kInkSoft, slop::kStrokeHair));
      }

      {
        char buf[20];
        snprintf(buf, sizeof(buf), "HUE %+.0f", hue * 180.f);
        float fw2 = slop::TextWidth(buf, 13.f);
        SkPoint hp = P(0, kBodyTop + 0.18_cm);
        slop::DrawText(canvas, buf, {hp.fX - fw2 / 2, hp.fY}, 13.f, slop::kInk, false, 0xC130);
      }

      std::string title = "COLOR";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -3.05_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xC140);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -3.32_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0xC141);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      float cy = kBodyTop - 0.05_cm;
      float d = std::hypot(pos.x, pos.y - cy);
      if (pos.y >= cy && d <= kWheelR + 0.15_cm && d >= kWheelR * 0.25f)
        return std::make_unique<ColorHueDrag>(p, *this);
      for (int i = 0; i < 4; ++i) {
        Rect s = SliderM(i);
        if (pos.x >= s.left - 0.2_cm && pos.x <= s.right + 0.2_cm && pos.y >= s.bottom - 0.12_cm &&
            pos.y <= s.top + 0.12_cm)
          return std::make_unique<ColorSliderDrag>(p, *this, i);
      }
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

ColorHueDrag::ColorHueDrag(ui::Pointer& p, ColorToy& w) : Action(p), widget(&w) {
  widget->dragging = 4;
}
ColorHueDrag::~ColorHueDrag() {
  if (widget) {
    widget->dragging = -1;
    widget->WakeAnimation();
  }
}
void ColorHueDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  float cy = ColorToy::kBodyTop - 0.05_cm;
  // Bearing from up: left half negative, right half positive; +-90 deg = +-1.0.
  float ang = std::atan2(pos.x, pos.y - cy);  // 0 = up, +right
  float f = std::clamp(ang / 1.5707963f, -1.f, 1.f);
  if (auto t = widget->LockColor()) {
    {
      auto lock = std::lock_guard(t->mutex);
      t->hue = f;
    }
    t->WakeToys();
  }
  widget->WakeAnimation();
}

ColorSliderDrag::ColorSliderDrag(ui::Pointer& p, ColorToy& w, int which)
    : Action(p), widget(&w), which(which) {
  widget->dragging = which;
}
ColorSliderDrag::~ColorSliderDrag() {
  if (widget) {
    widget->dragging = -1;
    widget->WakeAnimation();
  }
}
void ColorSliderDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect r = widget->SliderM(which);
  float t = std::clamp((pos.x - r.left) / std::max(1e-4f, r.Width()), 0.f, 1.f);
  float v = t * 2.f - 1.f;
  if (std::abs(v) < 0.04f) v = 0.f;  // the center detent
  if (auto c = widget->LockColor()) {
    {
      auto lock = std::lock_guard(c->mutex);
      if (which == 0)
        c->sat = v;
      else if (which == 1)
        c->r_shift = v;
      else if (which == 2)
        c->g_shift = v;
      else
        c->b_shift = v;
    }
    c->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Color::MakeToy(ui::Widget* parent) {
  return std::make_unique<ColorToy>(parent, *this);
}

// ============================================================================
// Seedfill
// ============================================================================
// A 3x3 seed block at the normalized (u,v) floods the binarized mask. If the
// seed lands on background, the mask is inverted in place so the pin grabs
// whatever is under it. Returns the filled component (1 bpp).
static Pix* SeedfillFill(Pix* mask1, float u, float v, int conn) {
  int w = pixGetWidth(mask1), h = pixGetHeight(mask1);
  if (w <= 0 || h <= 0) return nullptr;
  int sx = std::clamp((int)std::lround(u * (w - 1)), 0, w - 1);
  int sy = std::clamp((int)std::lround(v * (h - 1)), 0, h - 1);
  l_uint32 under = 0;
  if (pixGetPixel(mask1, sx, sy, &under) == 0 && under == 0) pixInvert(mask1, mask1);
  Pix* seed = pixCreate(w, h, 1);
  if (!seed) return nullptr;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx)
      pixSetPixel(seed, std::clamp(sx + dx, 0, w - 1), std::clamp(sy + dy, 0, h - 1), 1);
  Pix* fill = pixSeedfillBinary(nullptr, seed, mask1, conn);
  pixDestroy(&seed);
  return fill;
}

Pix* Seedfill::ApplyOp(Pix* in, const float*) const {
  float u, v;
  int conn;
  uint32_t rgb;
  bool maskmode;
  {
    auto lock = std::lock_guard(mutex);
    u = std::clamp(seed_u, 0.f, 1.f);
    v = std::clamp(seed_v, 0.f, 1.f);
    conn = eight ? 8 : 4;
    rgb = paint_rgb;
    maskmode = emit_mask;
  }
  Pix* mask = pixConvertTo1(in, 130);
  if (!mask) return nullptr;
  Pix* fill = SeedfillFill(mask, u, v, conn);
  pixDestroy(&mask);
  if (!fill) return nullptr;
  if (maskmode) return fill;  // 1 bpp component; preview + surface upconvert
  // PAINT mode: the input with the flooded component painted the chosen colour.
  Pix* out = pixCopy(nullptr, in);
  if (out) {
    l_uint32 paint = 0;
    composeRGBPixel((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff, &paint);
    pixSetMasked(out, fill, paint);
  }
  pixDestroy(&fill);
  return out;
}
void Seedfill::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("seed_u");
  writer.Double(seed_u);
  writer.Key("seed_v");
  writer.Double(seed_v);
  writer.Key("connectivity");
  writer.Int(eight ? 8 : 4);
  writer.Key("paint_rgb");
  writer.Int((int)paint_rgb);
  writer.Key("emit_mask");
  writer.Bool(emit_mask);
}
bool Seedfill::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  if (key == "seed_u") {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) seed_u = std::clamp((float)x, 0.f, 1.f);
  } else if (key == "seed_v") {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) seed_v = std::clamp((float)x, 0.f, 1.f);
  } else if (key == "connectivity") {
    int x = 0;
    d.Get(x, status);
    if (OK(status)) eight = (x != 4);
  } else if (key == "paint_rgb") {
    int x = 0;
    d.Get(x, status);
    if (OK(status)) paint_rgb = (uint32_t)x & 0xFFFFFF;
  } else if (key == "emit_mask") {
    bool x = false;
    d.Get(x, status);
    if (OK(status)) emit_mask = x;
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct SeedfillToy;
// Dragging (or click-placing) the seed pin on the preview.
struct SeedDrag : Action {
  TrackedPtr<SeedfillToy> widget;
  SeedDrag(ui::Pointer& p, SeedfillToy& w);
  ~SeedDrag();
  void Update() override;
};
struct ConnPoke : Action {
  ConnPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};

struct SeedfillToy : ObjectToy {
  sk_sp<SkImage> cached_preview;  // composite: mask in gray + filled component in cyan
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  float seed_u = 0.5f, seed_v = 0.5f;
  bool eight = true;
  uint32_t paint_rgb = 0xED1C24;
  bool emit_mask = false;
  bool dragging = false;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;
  constexpr static float kBottom = -4.15_cm;

  Ptr<Seedfill> LockSeed() const { return LockObject<Seedfill>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  SeedfillToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockSeed()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      seed_u = t->seed_u;
      seed_v = t->seed_v;
      eight = t->eight;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.85_cm, -1.35_cm, 2.85_cm, 0.7_cm); }
  Rect ConnM() const { return Rect(-2.7_cm, -2.4_cm, -1.05_cm, -1.75_cm); }
  Rect PaletteM() const { return Rect(-0.85_cm, -2.4_cm, 0.95_cm, -1.78_cm); }
  Rect ModeChipM(int which) const {
    return which == 0 ? Rect(1.02_cm, -2.06_cm, 1.72_cm, -1.76_cm)
                      : Rect(1.02_cm, -2.4_cm, 1.72_cm, -2.1_cm);
  }
  int PaletteIndex() const {
    for (int i = 0; i < ui::leptonica::kPaletteCount; ++i)
      if ((ui::leptonica::kPaletteColors[i] & 0xFFFFFF) == paint_rgb) return i;
    return -1;
  }

  // The displayed image's aspect-fitted rect inside PreviewM (same math as DrawPreviewFitted).
  Rect FittedRectM() const {
    Rect area = PreviewM();
    if (!cached_preview) return area;
    float iw = (float)cached_preview->width(), ih = (float)cached_preview->height();
    if (iw <= 0 || ih <= 0) return area;
    float s = std::min(area.Width() / iw, area.Height() / ih);
    return Rect::MakeCenter({area.CenterX(), area.CenterY()}, iw * s, ih * s);
  }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    b.addOval(Rect(-2.05_cm, 0.66_cm, 0.45_cm, 1.38_cm).sk);  // puddle
    SkPathBuilder stream;
    stream.moveTo(0.62_cm, 1.45_cm);
    stream.lineTo(0.92_cm, 1.45_cm);
    stream.lineTo(0.05_cm, 1.05_cm);
    stream.lineTo(-0.25_cm, 1.05_cm);
    stream.close();
    b.addPath(stream.detach());
    SkPath bucket = SkPath::Rect(Rect(0.75_cm, 1.35_cm, 2.15_cm, 2.35_cm).sk);
    b.addPath(bucket.makeTransform(SkMatrix::RotateDeg(35.f, {0.75_cm, 1.35_cm})));
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, 3.0_cm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, 3.4_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.0_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t SeedHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockSeed()) {
      auto lock = std::lock_guard(t->mutex);
      h ^= (uint32_t)std::lround(t->seed_u * 4096.f);
      h *= 16777619u;
      h ^= (uint32_t)std::lround(t->seed_v * 4096.f);
      h *= 16777619u;
      h ^= (uint32_t)(t->eight ? 1 : 0);
      h *= 16777619u;
      h ^= t->paint_rgb;
      h *= 16777619u;
      h ^= (uint32_t)(t->emit_mask ? 1 : 0);
      h *= 16777619u;
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;

    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    // Full resolution (cap 800, never upscale): downscaling before binarization changes
    // topology - a diagonal corner touch becomes an orthogonal bridge and 4-conn lies.
    float scl = std::min(1.f, std::min(800.f / sw, 800.f / sh));
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    sk_sp<SkImage> small = surf->makeImageSnapshot();
    if (!small) return;
    Pix* pix = SkImageToPix(small);
    if (!pix) return;
    float u, v;
    int conn;
    uint32_t prgb;
    bool pmask;
    {
      auto t = LockSeed();
      if (!t) {
        pixDestroy(&pix);
        return;
      }
      auto lock = std::lock_guard(t->mutex);
      u = std::clamp(t->seed_u, 0.f, 1.f);
      v = std::clamp(t->seed_v, 0.f, 1.f);
      conn = t->eight ? 8 : 4;
      prgb = t->paint_rgb;
      pmask = t->emit_mask;
    }
    Pix* mask = pixConvertTo1(pix, 130);
    if (!mask) {
      pixDestroy(&pix);
      return;
    }
    Pix* fill = SeedfillFill(mask, u, v, conn);
    if (!fill) {
      pixDestroy(&mask);
      pixDestroy(&pix);
      return;
    }

    l_uint32 paint = 0;
    composeRGBPixel((prgb >> 16) & 0xff, (prgb >> 8) & 0xff, prgb & 0xff, &paint);
    if (pmask) {
      // MASK mode: the gray composite - every blob visible, the component in the chosen paint.
      out_depth = 1;
      out_cmap = false;
      Pix* disp = pixCreate(pixGetWidth(mask), pixGetHeight(mask), 32);
      if (disp) {
        pixSetAll(disp);
        l_uint32 gray = 0;
        composeRGBPixel(0xC9, 0xC2, 0xB2, &gray);
        pixSetMasked(disp, mask, gray);
        pixSetMasked(disp, fill, paint);
        cached_preview = PixToSkImage(disp);
        pixDestroy(&disp);
      }
    } else {
      // PAINT mode: the genuine output - the input with the component painted.
      out_depth = 32;
      out_cmap = false;
      Pix* out = pixCopy(nullptr, pix);
      if (out) {
        pixSetMasked(out, fill, paint);
        cached_preview = PixToSkImage(out);
        pixDestroy(&out);
      }
    }
    pixDestroy(&pix);
    pixDestroy(&mask);
    pixDestroy(&fill);
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockSeed()) {
      uint32_t h = SeedHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        seed_u = t->seed_u;
        seed_v = t->seed_v;
        eight = t->eight;
        paint_rgb = t->paint_rgb;
        emit_mask = t->emit_mask;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    {
      SkPaint pourp;
      pourp.setAntiAlias(true);
      pourp.setColor(paint_rgb | 0xFF000000);
      canvas.drawOval(Rect(-1.93_cm, 0.74_cm, 0.33_cm, 1.28_cm).sk, pourp);
      SkPathBuilder stream;
      stream.moveTo(0.62_cm, 1.38_cm);
      stream.lineTo(0.84_cm, 1.38_cm);
      stream.lineTo(0.0_cm, 1.0_cm);
      stream.lineTo(-0.2_cm, 1.0_cm);
      stream.close();
      canvas.drawPath(stream.detach(), pourp);
      SkPath mouth = SkPath::Rect(Rect(0.81_cm, 1.47_cm, 1.03_cm, 2.23_cm).sk);
      canvas.drawPath(mouth.makeTransform(SkMatrix::RotateDeg(35.f, {0.75_cm, 1.35_cm})), pourp);
    }

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    if (cached_preview) {
      Rect fit = FittedRectM();
      float px = fit.left + std::clamp(seed_u, 0.f, 1.f) * fit.Width();
      float py = fit.top - std::clamp(seed_v, 0.f, 1.f) * fit.Height();
      SkPaint stem;
      stem.setAntiAlias(true);
      stem.setStyle(SkPaint::kStroke_Style);
      stem.setStrokeWidth(0.05_cm);
      stem.setColor("#1a1a1a"_color);
      canvas.drawLine(px, py, px + 0.18_cm, py + 0.5_cm, stem);
      SkPaint head;
      head.setAntiAlias(true);
      head.setColor("#ed1c24"_color);
      canvas.drawCircle(px + 0.18_cm, py + 0.5_cm, 0.15_cm, head);
      SkPaint headring;
      headring.setAntiAlias(true);
      headring.setStyle(SkPaint::kStroke_Style);
      headring.setStrokeWidth(0.035_cm);
      headring.setColor("#1a1a1a"_color);
      canvas.drawCircle(px + 0.18_cm, py + 0.5_cm, 0.15_cm, headring);
      SkPaint dot;
      dot.setAntiAlias(true);
      dot.setColor("#1a1a1a"_color);
      canvas.drawCircle(px, py, 0.045_cm, dot);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0x5F01, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0x5F90);

      {
        SkPoint a = P(1.90_cm, 2.15_cm), b2 = P(1.32_cm, 2.97_cm);  // base side corners
        SkPoint mid = {(a.fX + b2.fX) * 0.5f, (a.fY + b2.fY) * 0.5f};
        SkPoint n = {std::cos(35.f * 0.0174533f), -std::sin(35.f * 0.0174533f)};  // outward
        float bow_px = 0.5_cm / kPxToMetric;
        SkPathBuilder arc;
        for (int i = 0; i <= 12; ++i) {
          float along = ((float)i / 12.f - 0.5f) * 1.7f;  // -0.85..0.85 of the half-chord
          float bow = std::sqrt(std::max(0.f, 1.f - along * along));
          SkPoint pt{mid.fX + (a.fX - mid.fX) * along + n.fX * bow * bow_px,
                     mid.fY + (a.fY - mid.fY) * along + n.fY * bow * bow_px};
          if (i == 0)
            arc.moveTo(pt);
          else
            arc.lineTo(pt);
        }
        slop::SketchyStroke(canvas, arc.detach(), slop::kInk, slop::kStroke, 0x5F05, 1);
      }

      ui::leptonica::DrawConnectivity(canvas, RPX(ConnM()), eight, slop::State::Default, 0x5F10);
      slop::DrawText(canvas, "CONNECTIVITY", P(ConnM().left, ConnM().top + 0.14_cm), 13.f,
                     kLabelInk, false, 0);

      ui::leptonica::DrawPalette(canvas, RPX(PaletteM()), PaletteIndex(), slop::State::Default,
                                 0x5FA0);
      slop::DrawText(canvas, "PAINT", P(PaletteM().left, PaletteM().top + 0.14_cm), 13.f, kLabelInk,
                     false, 0);

      for (int which = 0; which < 2; ++which) {
        SkRect chip = RPX(ModeChipM(which));
        bool sel = (which == 1) == emit_mask;
        SkPath cp = slop::WonkyRoundRect(chip, chip.height() * 0.3f, slop::kWonk * 0.5f,
                                         0x5FB0u + (uint32_t)which);
        if (sel) slop::HandShadow(canvas, cp, {2.f, 2.f}, slop::kShadow, 0x5FB2u + (uint32_t)which);
        slop::MisregFill(canvas, cp, sel ? slop::kPaper : slop::kGray, 0x5FB4u + (uint32_t)which);
        slop::SketchyStroke(canvas, cp, slop::kInk, sel ? slop::kStroke : slop::kStrokeHair,
                            0x5FB6u + (uint32_t)which, 1);
        const char* lab = which == 0 ? "PAINT" : "MASK";
        float lw = slop::TextWidth(lab, 11.f);
        slop::DrawText(canvas, lab, {chip.centerX() - lw / 2, chip.centerY() + 4.f}, 11.f,
                       sel ? slop::kInk : slop::kInkSoft, false, 0);
        if (sel) slop::Highlight(canvas, chip, slop::kBlue, 0x5FB8);
      }

      std::string title = "SEEDFILL";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -3.0_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0x5F30);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -3.28_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0x5F31);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      {
        SkPoint ppx{pos.x / kPxToMetric, -pos.y / kPxToMetric};
        Rect cm = ConnM();
        SkRect cellr = SkRect::MakeLTRB(cm.left / kPxToMetric, -cm.top / kPxToMetric,
                                        cm.right / kPxToMetric, -cm.bottom / kPxToMetric);
        int hit = ui::leptonica::ConnectivityHit(cellr, ppx);
        if (hit != 0) {
          if (auto t = LockSeed()) {
            {
              auto lock = std::lock_guard(t->mutex);
              t->eight = hit == 8;
            }
            t->WakeToys();
          }
          return std::make_unique<ConnPoke>(p);
        }
      }
      {
        SkPoint ppx{pos.x / kPxToMetric, -pos.y / kPxToMetric};
        Rect pm = PaletteM();
        SkRect pr = SkRect::MakeLTRB(pm.left / kPxToMetric, -pm.top / kPxToMetric,
                                     pm.right / kPxToMetric, -pm.bottom / kPxToMetric);
        int hit = ui::leptonica::PaletteHit(pr, ppx);
        if (hit >= 0) {
          if (auto t = LockSeed()) {
            {
              auto lock = std::lock_guard(t->mutex);
              t->paint_rgb = ui::leptonica::kPaletteColors[hit] & 0xFFFFFF;
            }
            t->WakeToys();
          }
          return std::make_unique<ConnPoke>(p);
        }
      }
      for (int which = 0; which < 2; ++which) {
        Rect r = ModeChipM(which);
        if (pos.x >= r.left && pos.x <= r.right && pos.y >= r.bottom && pos.y <= r.top) {
          if (auto t = LockSeed()) {
            {
              auto lock = std::lock_guard(t->mutex);
              t->emit_mask = which == 1;
            }
            t->WakeToys();
          }
          return std::make_unique<ConnPoke>(p);
        }
      }
      Rect fit = FittedRectM();
      if (pos.x >= fit.left - 0.1_cm && pos.x <= fit.right + 0.1_cm &&
          pos.y >= fit.bottom - 0.1_cm && pos.y <= fit.top + 0.1_cm)
        return std::make_unique<SeedDrag>(p, *this);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

static void SeedfillSetFromPointer(SeedfillToy& w, ui::Pointer& pointer) {
  Vec2 pos = pointer.PositionWithin(w);
  Rect fit = w.FittedRectM();
  if (fit.Width() <= 0 || fit.Height() <= 0) return;
  float u = std::clamp((pos.x - fit.left) / fit.Width(), 0.f, 1.f);
  float v = std::clamp((fit.top - pos.y) / fit.Height(), 0.f, 1.f);
  if (auto t = w.LockSeed()) {
    {
      auto lock = std::lock_guard(t->mutex);
      t->seed_u = u;
      t->seed_v = v;
    }
    t->WakeToys();
  }
  w.WakeAnimation();
}

SeedDrag::SeedDrag(ui::Pointer& p, SeedfillToy& w) : Action(p), widget(&w) {
  widget->dragging = true;
  SeedfillSetFromPointer(w, p);  // click places the seed immediately
}
SeedDrag::~SeedDrag() {
  if (widget) {
    widget->dragging = false;
    widget->WakeAnimation();
  }
}
void SeedDrag::Update() {
  if (widget) SeedfillSetFromPointer(*widget, pointer);
}

std::unique_ptr<ObjectToy> Seedfill::MakeToy(ui::Widget* parent) {
  return std::make_unique<SeedfillToy>(parent, *this);
}

// ============================================================================
// Generate
// ============================================================================

Pix* Generate::ApplyOp(Pix* in, const float*) const {
  int m, sc, sd;
  {
    auto lock = std::lock_guard(mutex);
    m = mode;
    sc = std::clamp(scale, 1, 4);
    sd = std::clamp(stdev, 2, 80);
  }
  if (m == 1) return pixAddGaussianNoise(in, (l_float32)sd);
  return pixMakeGamutRGB(sc);  // replaces the sheet (the image adopts the chart's size)
}

void Generate::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("mode");
  writer.Int(mode);
  writer.Key("scale");
  writer.Int(scale);
  writer.Key("stdev");
  writer.Int(stdev);
}

bool Generate::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  int v = 0;
  if (key == "mode") {
    d.Get(v, status);
    if (OK(status)) mode = std::clamp(v, 0, 1);
  } else if (key == "scale") {
    d.Get(v, status);
    if (OK(status)) scale = std::clamp(v, 1, 4);
  } else if (key == "stdev") {
    d.Get(v, status);
    if (OK(status)) stdev = std::clamp(v, 2, 80);
  } else {
    return false;
  }
  WakeToys();
  return true;
}

struct GenerateToy;
struct GenPoke : Action {
  GenPoke(ui::Pointer& p) : Action(p) {}
  void Update() override {}
};
struct GenSliderDrag : Action {
  TrackedPtr<GenerateToy> widget;
  GenSliderDrag(ui::Pointer& p, GenerateToy& w);
  void Update() override;
};

struct GenerateToy : ObjectToy {
  sk_sp<SkImage> cached_preview;
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int out_depth = 0;
  bool out_cmap = false;

  int mode = 0, scale = 2, stdev = 20;

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;
  constexpr static float kBottom = -4.15_cm;

  Ptr<Generate> LockGen() const { return LockObject<Generate>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  GenerateToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockGen()) {
      auto lock = std::lock_guard(t->mutex);
      mode = t->mode;
      scale = t->scale;
      stdev = t->stdev;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.85_cm, -1.0_cm, 2.85_cm, 0.85_cm); }
  SkPoint ModeWheelCM() const { return {1.9_cm, -1.75_cm}; }
  constexpr static float kModeWheelR = 0.55_cm;
  Rect SliderM() const { return Rect(-2.7_cm, -2.1_cm, 0.6_cm, -1.75_cm); }

  static SkMatrix CanFrame() { return SkMatrix::RotateDeg(-10.f, {-1.7_cm, kBodyTop}); }
  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    SkPath can = SkPath::RRect(
        RRect::MakeSimple(Rect(-2.25_cm, kBodyTop - 0.1_cm, -1.15_cm, kBodyTop + 1.5_cm), 1_mm).sk);
    b.addPath(can.makeTransform(CanFrame()));
    SkPath cap = SkPath::Rect(Rect(-2.0_cm, kBodyTop + 1.45_cm, -1.4_cm, kBodyTop + 1.8_cm).sk);
    b.addPath(cap.makeTransform(CanFrame()));
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop + 1.8_cm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, kBodyTop + 2.6_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.3_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t GenHash() const {
    uint32_t h = 2166136261u;
    auto mix = [&](uint32_t v) {
      h ^= v;
      h *= 16777619u;
    };
    if (auto t = LockGen()) {
      auto lock = std::lock_guard(t->mutex);
      mix((uint32_t)t->mode);
      mix((uint32_t)t->scale);
      mix((uint32_t)t->stdev);
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    // A generator previews even with nothing connected: synth onto a small blank sheet.
    Pix* pix = nullptr;
    if (source) {
      int sw = source->width(), sh = source->height();
      float scl = std::min(1.f, std::min(520.f / sw, 520.f / sh));
      int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
      auto surf = SkSurfaces::Raster(
          SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
      if (!surf) return;
      surf->getCanvas()->drawImageRect(
          source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
          SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
      sk_sp<SkImage> small = surf->makeImageSnapshot();
      if (small) pix = SkImageToPix(small);
    } else {
      pix = MakeBlankPix(480, 320, 0xffffff00);
    }
    if (!pix) return;
    Pix* out = nullptr;
    if (auto gen = LockGen()) out = gen->ApplyOp(pix, nullptr);
    if (out && out != pix) {
      pixDestroy(&pix);
      pix = out;
    }
    if (pix) {
      out_depth = pixGetDepth(pix);
      out_cmap = pixGetColormap(pix) != nullptr;
      cached_preview = PixToSkImage(pix);
      pixDestroy(&pix);
    }
  }

  animation::Phase Tick(time::Timer&) override {
    if (auto t = LockGen()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      mode = t->mode;
      scale = t->scale;
      stdev = t->stdev;
    }
    uint32_t h = GenHash();
    auto tool = LockTool();
    uint32_t live_src = 0;
    if (tool) {
      auto nested = tool->image->FindInterface();
      Object* owner = nested.Owner<Object>();
      ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
      live_src = SourceImageId(ip ? ip.GetImage() : nullptr);
    }
    if (h != preview_hash || live_src != preview_source_id || preview_dirty) {
      preview_hash = h;
      RecomputePreview();
      preview_dirty = false;
    }
    return animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0x6E01, 1);
      DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0x6E90);

      {
        SkPath band =
            SkPath::Rect(Rect(-2.13_cm, kBodyTop + 0.35_cm, -1.27_cm, kBodyTop + 0.95_cm).sk);
        SkPath band_px = band.makeTransform(CanFrame())
                             .makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
        slop::MisregFill(canvas, band_px, slop::kCyan, 0x6E10);
        slop::SketchyStroke(canvas, band_px, slop::kInk, slop::kStrokeHair, 0x6E11, 1);
        static const SkPoint kDrops[5] = {{-0.9_cm, kBodyTop + 1.9_cm},
                                          {-0.45_cm, kBodyTop + 1.65_cm},
                                          {-0.55_cm, kBodyTop + 2.15_cm},
                                          {-0.1_cm, kBodyTop + 1.9_cm},
                                          {0.05_cm, kBodyTop + 1.55_cm}};
        for (int i = 0; i < 5; ++i) {
          SkPoint d = P(kDrops[i].fX, kDrops[i].fY);
          canvas.drawPath(slop::WobbleEllipse(d, 4.5f - i * 0.5f, 4.f - i * 0.5f,
                                              slop::kWonk * 0.5f, 0x6E20u + (uint32_t)i, 10),
                          slop::InkPaint(slop::kInk, slop::kStrokeHair));
        }
      }

      const char* kModes[2] = {"GAMUT", "NOISE"};
      ui::leptonica::DrawModeWheel(
          canvas, {ModeWheelCM().fX / kPxToMetric, -ModeWheelCM().fY / kPxToMetric},
          kModeWheelR / kPxToMetric, kModes, 2, mode, slop::State::Default, 0x6E30);

      {
        Rect s = SliderM();
        char buf[24];
        float frac;
        if (mode == 0) {
          frac = (scale - 1) / 3.f;
          snprintf(buf, sizeof(buf), "SCALE %d", scale);
        } else {
          frac = (stdev - 2) / 78.f;
          snprintf(buf, sizeof(buf), "STDEV %d", stdev);
        }
        slop::Slider(canvas, RPX(s), std::clamp(frac, 0.f, 1.f), slop::State::Default, 0x6E40);
        slop::DrawText(canvas, buf, P(s.left, s.top + 0.14_cm), 15.f, kLabelInk, false, 0);
      }

      std::string title = "GENERATE";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -2.85_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0x6E50);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -3.13_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0x6E51);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint pp = {pos.x / kPxToMetric, -pos.y / kPxToMetric};
      int m = ui::leptonica::ModeWheelHit(
          {ModeWheelCM().fX / kPxToMetric, -ModeWheelCM().fY / kPxToMetric},
          kModeWheelR / kPxToMetric, pp, 2);
      if (m >= 0) {
        if (auto t = LockGen()) {
          {
            auto lock = std::lock_guard(t->mutex);
            t->mode = m;
          }
          t->WakeToys();
        }
        return std::make_unique<GenPoke>(p);
      }
      Rect s = SliderM();
      if (pos.x >= s.left - 0.2_cm && pos.x <= s.right + 0.2_cm && pos.y >= s.bottom - 0.2_cm &&
          pos.y <= s.top + 0.2_cm)
        return std::make_unique<GenSliderDrag>(p, *this);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

GenSliderDrag::GenSliderDrag(ui::Pointer& p, GenerateToy& w) : Action(p), widget(&w) {}
void GenSliderDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect s = widget->SliderM();
  float t = std::clamp((pos.x - s.left) / std::max(1e-4f, s.Width()), 0.f, 1.f);
  if (auto g = widget->LockGen()) {
    {
      auto lock = std::lock_guard(g->mutex);
      if (g->mode == 0)
        g->scale = 1 + (int)std::lround(t * 3.f);
      else
        g->stdev = 2 + (int)std::lround(t * 78.f);
    }
    g->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> Generate::MakeToy(ui::Widget* parent) {
  return std::make_unique<GenerateToy>(parent, *this);
}

// ============================================================================
// Crop
// ============================================================================
// The preview shows the source with the marquee on it; the output is the
// kept part.
Pix* CropRegion::ApplyOp(Pix* in, const float*) const {
  float a0, b0, a1, b1;
  {
    auto lock = std::lock_guard(mutex);
    a0 = u0;
    b0 = v0;
    a1 = u1;
    b1 = v1;
  }
  int w = pixGetWidth(in), h = pixGetHeight(in);
  if (w <= 0 || h <= 0) return nullptr;
  int x = std::clamp((int)std::lround(a0 * w), 0, w - 1);
  int y = std::clamp((int)std::lround(b0 * h), 0, h - 1);
  int bw = std::clamp((int)std::lround((a1 - a0) * w), 1, w - x);
  int bh = std::clamp((int)std::lround((b1 - b0) * h), 1, h - y);
  BOX* box = boxCreate(x, y, bw, bh);
  if (!box) return nullptr;
  Pix* out = pixClipRectangle(in, box, nullptr);
  boxDestroy(&box);
  return out;
}
void CropRegion::SerializeState(ObjectSerializer& writer) const {
  auto lock = std::lock_guard(mutex);
  writer.Key("u0");
  writer.Double(u0);
  writer.Key("v0");
  writer.Double(v0);
  writer.Key("u1");
  writer.Double(u1);
  writer.Key("v1");
  writer.Double(v1);
}
bool CropRegion::DeserializeKey(ObjectDeserializer& d, StrView key) {
  Status status;
  auto lock = std::lock_guard(mutex);
  auto get01 = [&](float& dst) {
    double x = 0;
    d.Get(x, status);
    if (OK(status)) dst = std::clamp((float)x, 0.f, 1.f);
  };
  if (key == "u0") {
    get01(u0);
  } else if (key == "v0") {
    get01(v0);
  } else if (key == "u1") {
    get01(u1);
  } else if (key == "v1") {
    get01(v1);
  } else if (key == "x_min") {  // legacy brass-Crop keys (y was measured UP)
    get01(u0);
  } else if (key == "x_max") {
    get01(u1);
  } else if (key == "y_min") {
    float t = 0;
    get01(t);
    v1 = 1.f - t;
  } else if (key == "y_max") {
    float t = 0;
    get01(t);
    v0 = 1.f - t;
  } else {
    return PhotoTool::DeserializeKey(d, key);
  }
  return true;
}

struct CropToy;
// Dragging a marquee corner (which 1..4 = TL/TR/BL/BR) or the whole box (which 5).
struct CropDrag : Action {
  TrackedPtr<CropToy> widget;
  int which;
  float grab_du = 0.f, grab_dv = 0.f;  // pointer offset from the box's top-left (move mode)
  CropDrag(ui::Pointer& p, CropToy& w, int which);
  ~CropDrag();
  void Update() override;
};

struct CropToy : ObjectToy {
  sk_sp<SkImage> cached_preview;  // the SOURCE proxy (the marquee rides on it)
  uint32_t preview_hash = 0;
  uint32_t preview_source_id = 0;
  bool preview_dirty = true;
  int real_w = 0, real_h = 0;  // true source dims - the size readout speaks in real pixels
  int out_depth = 0;
  bool out_cmap = false;

  float u0 = 0.15f, v0 = 0.15f, u1 = 0.85f, v1 = 0.85f;
  int dragging = 0;  // CropDrag's `which` while live

  std::unique_ptr<ui::slop::RunButton> glass;

  std::string fn_credit;

  constexpr static float kHalfW = 3.1_cm;
  constexpr static float kBodyTop = 0.95_cm;
  constexpr static float kBottom = -4.15_cm;

  Ptr<CropRegion> LockCrop() const { return LockObject<CropRegion>(); }
  Ptr<PhotoTool> LockTool() const { return LockObject<PhotoTool>(); }

  CropToy(ui::Widget* parent, Object& obj) : ObjectToy(parent, obj) {
    glass = std::make_unique<ui::slop::RunButton>(this, [this] {
      if (auto t = LockObject<PhotoTool>()) t->develop->ScheduleRun();
    });
    if (auto t = LockCrop()) {
      fn_credit = std::string(t->LeptonicaFn());
      auto lock = std::lock_guard(t->mutex);
      u0 = t->u0;
      v0 = t->v0;
      u1 = t->u1;
      v1 = t->v1;
    }
  }

  bool CenteredAtZero() const override { return true; }

  Rect PreviewM() const { return Rect(-2.85_cm, -1.35_cm, 2.85_cm, 0.75_cm); }

  Rect FittedRectM() const {
    Rect area = PreviewM();
    if (!cached_preview) return area;
    float iw = (float)cached_preview->width(), ih = (float)cached_preview->height();
    if (iw <= 0 || ih <= 0) return area;
    float s = std::min(area.Width() / iw, area.Height() / ih);
    return Rect::MakeCenter({area.CenterX(), area.CenterY()}, iw * s, ih * s);
  }

  SkPath Shape() const override {
    SkPathBuilder b;
    b.addRect(Rect(-kHalfW, kBottom, kHalfW, kBodyTop).sk);
    b.addRect(Rect(-1.35_cm, 0.9_cm, -0.95_cm, 2.15_cm).sk);
    b.addRect(Rect(-1.35_cm, 1.78_cm, 0.35_cm, 2.15_cm).sk);
    b.addRect(Rect(0.8_cm, 0.9_cm, 1.2_cm, 1.6_cm).sk);
    b.addRect(Rect(-0.45_cm, 0.9_cm, 1.2_cm, 1.3_cm).sk);
    SkPath out;
    if (Simplify(b.detach(), &out)) return out;
    return SkPath::Rect(Rect(-kHalfW, kBottom, kHalfW, 2.15_cm).sk);
  }
  Optional<Rect> TextureBounds() const override {
    return Rect(-kHalfW - 0.5_cm, kBottom - 0.4_cm, kHalfW + 0.5_cm, 2.6_cm);
  }

  Vec2AndDir ArgStart(const Interface::Table& arg) override {
    if (&arg == static_cast<const Interface::Table*>(&PhotoTool::image_tbl))
      return {.pos = {-kHalfW, 0.0_cm}, .dir = 180_deg};
    return ui::slop::RunButton::AdjustArgStart(ObjectToy::ArgStart(arg));
  }

  uint32_t CropHash() const {
    uint32_t h = 2166136261u;
    if (auto t = LockCrop()) {
      auto lock = std::lock_guard(t->mutex);
      for (float f : {t->u0, t->v0, t->u1, t->v1}) {
        h ^= (uint32_t)std::lround(f * 4096.f);
        h *= 16777619u;
      }
    }
    return h;
  }

  void RecomputePreview() {
    auto tool = LockTool();
    cached_preview = nullptr;
    if (!tool) return;
    auto nested = tool->image->FindInterface();
    Object* owner = nested.Owner<Object>();
    ImageProvider ip = owner ? owner->As<ImageProvider>() : ImageProvider{};
    sk_sp<SkImage> source = ip ? ip.GetImage() : nullptr;
    preview_source_id = SourceImageId(source);
    if (!source) return;
    int sw = source->width(), sh = source->height();
    if (sw <= 0 || sh <= 0) return;
    real_w = sw;
    real_h = sh;
    float scl = std::min(300.f / sw, 300.f / sh);
    int dw = std::max(1, (int)(sw * scl)), dh = std::max(1, (int)(sh * scl));
    auto surf = SkSurfaces::Raster(
        SkImageInfo::Make(dw, dh, kRGBA_8888_SkColorType, SkAlphaType::kUnpremul_SkAlphaType));
    if (!surf) return;
    surf->getCanvas()->drawImageRect(
        source, SkRect::Make(SkISize{sw, sh}), SkRect::Make(SkISize{dw, dh}),
        SkSamplingOptions(SkFilterMode::kLinear), nullptr, SkCanvas::kFast_SrcRectConstraint);
    cached_preview = surf->makeImageSnapshot();
    // Run the real clip on the proxy for the depth chip (and to prove the box math).
    if (cached_preview) {
      Pix* pix = SkImageToPix(cached_preview);
      if (pix) {
        Pix* result = tool->ApplyOp(pix, nullptr);
        pixDestroy(&pix);
        if (result) {
          out_depth = pixGetDepth(result);
          out_cmap = pixGetColormap(result) != nullptr;
          pixDestroy(&result);
        }
      }
    }
    preview_dirty = false;
  }

  animation::Phase Tick(time::Timer&) override {
    bool has_source = false;
    if (auto t = LockCrop()) {
      uint32_t h = CropHash();
      if (h != preview_hash) {
        preview_hash = h;
        preview_dirty = true;
      }
      if (PhotoToolSourceId(*t) != preview_source_id) preview_dirty = true;
      if (preview_dirty) RecomputePreview();
      {
        auto lock = std::lock_guard(t->mutex);
        u0 = t->u0;
        v0 = t->v0;
        u1 = t->u1;
        v1 = t->v1;
      }
      fn_credit = std::string(t->LeptonicaFn());
      has_source = preview_source_id != 0;
    }
    return has_source ? animation::Animating : animation::Finished;
  }

  void Draw(SkCanvas& canvas) const override {
    SkPath shape = Shape();
    SkPaint image;
    image.setAntiAlias(true);
    image.setColor("#f4efe4"_color);
    canvas.drawPath(shape, image);

    if (cached_preview) {
      DrawPreviewFitted(canvas, cached_preview, PreviewM());
    } else {
      SkPaint ph;
      ph.setAntiAlias(true);
      ph.setColor("#e7dfcd"_color);
      canvas.drawRect(PreviewM().sk, ph);
    }

    {
      SlopHere g(canvas, {0, 0});
      auto P = [&](float a, float c) { return SkPoint{a / kPxToMetric, -c / kPxToMetric}; };
      auto RPX = [&](const Rect& r) {
        return SkRect::MakeLTRB(r.left / kPxToMetric, -r.top / kPxToMetric, r.right / kPxToMetric,
                                -r.bottom / kPxToMetric);
      };
      SkPath shape_px = shape.makeTransform(SkMatrix::Scale(1.f / kPxToMetric, -1.f / kPxToMetric));
      slop::SketchyStroke(canvas, shape_px, slop::kInk, slop::kStroke, 0xC701, 1);

      {
        SkRect ap = RPX(Rect(-0.88_cm, 1.35_cm, 0.73_cm, 1.71_cm));
        const float dash = 7.f, gap = 5.f;
        auto edge = [&](SkPoint a, SkPoint b, uint32_t es) {
          float len = std::hypot(b.fX - a.fX, b.fY - a.fY);
          int n = std::max(1, (int)(len / (dash + gap)));
          for (int i = 0; i < n; ++i) {
            float t0 = (i * (dash + gap)) / len, t1 = std::min(1.f, t0 + dash / len);
            SkPoint p0{a.fX + (b.fX - a.fX) * t0, a.fY + (b.fY - a.fY) * t0};
            SkPoint p1{a.fX + (b.fX - a.fX) * t1, a.fY + (b.fY - a.fY) * t1};
            canvas.drawPath(slop::WobbleLine(p0, p1, 0.8f, 5.f, slop::Hash2(es, (uint32_t)i)),
                            slop::InkPaint("#f4efe4"_color, slop::kStrokeHair));
          }
        };
        edge({ap.fLeft, ap.fTop}, {ap.fRight, ap.fTop}, 0xC730);
        edge({ap.fRight, ap.fTop}, {ap.fRight, ap.fBottom}, 0xC731);
        edge({ap.fRight, ap.fBottom}, {ap.fLeft, ap.fBottom}, 0xC732);
        edge({ap.fLeft, ap.fBottom}, {ap.fLeft, ap.fTop}, 0xC733);
      }

      if (cached_preview) {
        SkRect fit = RPX(FittedRectM());
        SkRect mq = SkRect::MakeLTRB(fit.fLeft + u0 * fit.width(), fit.fTop + v0 * fit.height(),
                                     fit.fLeft + u1 * fit.width(), fit.fTop + v1 * fit.height());
        ui::leptonica::DrawRegion(canvas, fit, mq,
                                  dragging ? slop::State::Pressed : slop::State::Default, 0xC710);
        DrawDepthChipPx(canvas, RPX(PreviewM()), cached_preview, out_depth, out_cmap, 0xC790);
      }

      // The crop's output size in true source pixels, not preview pixels.
      if (real_w > 0 && real_h > 0) {
        int ow = std::max(1, (int)std::lround((u1 - u0) * real_w));
        int oh = std::max(1, (int)std::lround((v1 - v0) * real_h));
        char buf[40];
        snprintf(buf, sizeof(buf), "KEEP %d x %d px", ow, oh);
        slop::DrawText(canvas, buf, P(-2.7_cm, -1.78_cm), 14.f, kLabelInk, false, 0);
      }

      std::string title = "CROP";
      float tw = slop::TextWidth(title, kTitleTextPx);
      SkPoint tc = P(0, -3.0_cm);
      slop::DrawText(canvas, title, {tc.fX - tw * 0.5f, tc.fY}, kTitleTextPx, slop::kInk, true,
                     0xC720);
      if (!fn_credit.empty()) {
        std::string credit = fn_credit + "()";
        float fw = slop::TextWidth(credit, kCreditTextPx);
        SkPoint fc = P(0, -3.28_cm);
        slop::DrawText(canvas, credit, {fc.fX - fw * 0.5f, fc.fY}, kCreditTextPx, "#8a7d66"_color,
                       false, 0xC721);
      }
    }

    DrawChildren(canvas);
  }

  std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override {
    if (btn == ui::PointerButton::Left && cached_preview) {
      Vec2 pos = p.PositionWithin(*this);
      SkPoint ppx{pos.x / kPxToMetric, -pos.y / kPxToMetric};
      Rect fm = FittedRectM();
      SkRect fit = SkRect::MakeLTRB(fm.left / kPxToMetric, -fm.top / kPxToMetric,
                                    fm.right / kPxToMetric, -fm.bottom / kPxToMetric);
      SkRect mq = SkRect::MakeLTRB(fit.fLeft + u0 * fit.width(), fit.fTop + v0 * fit.height(),
                                   fit.fLeft + u1 * fit.width(), fit.fTop + v1 * fit.height());
      int hit = ui::leptonica::RegionHit(mq, ppx);
      if (hit != 0) return std::make_unique<CropDrag>(p, *this, hit);
    }
    return ObjectToy::FindAction(p, btn);
  }

  void FillChildren(Vec<Widget*>& children) override {
    if (glass) children.push_back(glass.get());
  }
};

CropDrag::CropDrag(ui::Pointer& p, CropToy& w, int which) : Action(p), widget(&w), which(which) {
  widget->dragging = which;
  if (which == 5) {
    Vec2 pos = p.PositionWithin(w);
    Rect fit = w.FittedRectM();
    if (fit.Width() > 0 && fit.Height() > 0) {
      grab_du = (pos.x - fit.left) / fit.Width() - w.u0;
      grab_dv = (fit.top - pos.y) / fit.Height() - w.v0;
    }
  }
}
CropDrag::~CropDrag() {
  if (widget) {
    widget->dragging = 0;
    widget->WakeAnimation();
  }
}
void CropDrag::Update() {
  if (!widget) return;
  Vec2 pos = pointer.PositionWithin(*widget);
  Rect fit = widget->FittedRectM();
  if (fit.Width() <= 0 || fit.Height() <= 0) return;
  float u = std::clamp((pos.x - fit.left) / fit.Width(), 0.f, 1.f);
  float v = std::clamp((fit.top - pos.y) / fit.Height(), 0.f, 1.f);
  constexpr float kMin = 0.02f;
  if (auto t = widget->LockCrop()) {
    {
      auto lock = std::lock_guard(t->mutex);
      switch (which) {
        case 1:  // TL
          t->u0 = std::min(u, t->u1 - kMin);
          t->v0 = std::min(v, t->v1 - kMin);
          break;
        case 2:  // TR
          t->u1 = std::max(u, t->u0 + kMin);
          t->v0 = std::min(v, t->v1 - kMin);
          break;
        case 3:  // BL
          t->u0 = std::min(u, t->u1 - kMin);
          t->v1 = std::max(v, t->v0 + kMin);
          break;
        case 4:  // BR
          t->u1 = std::max(u, t->u0 + kMin);
          t->v1 = std::max(v, t->v0 + kMin);
          break;
        case 5: {  // move, size-preserving
          float w0 = t->u1 - t->u0, h0 = t->v1 - t->v0;
          float nu0 = std::clamp(u - grab_du, 0.f, 1.f - w0);
          float nv0 = std::clamp(v - grab_dv, 0.f, 1.f - h0);
          t->u0 = nu0;
          t->v0 = nv0;
          t->u1 = nu0 + w0;
          t->v1 = nv0 + h0;
          break;
        }
      }
    }
    t->WakeToys();
  }
  widget->WakeAnimation();
}

std::unique_ptr<ObjectToy> CropRegion::MakeToy(ui::Widget* parent) {
  return std::make_unique<CropToy>(parent, *this);
}
std::unique_ptr<ObjectToy> LeptonicaShelf::MakeToy(ui::Widget* parent) {
  return std::make_unique<LeptonicaShelfWidget>(parent, *this);
}

std::unique_ptr<ObjectToy> PhotoTool::MakeToy(ui::Widget* parent) {
  return std::make_unique<PhotoToolWidget>(parent, *this);
}

}  // namespace automat::library
