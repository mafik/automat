#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

#include <include/core/SkImage.h>

#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "base.hpp"
#include "image_provider.hpp"
#include "span.hpp"
#include "str.hpp"
#include "units.hpp"

class SkCanvas;
struct Pix;

namespace automat::library {

// Port icons show what kind of data plugs in. ~1x1cm paths; ConnectionWidget
// scales them onto the plug face.
std::unique_ptr<ui::Widget> MakePhotoPortIcon(ui::Widget* parent);
std::unique_ptr<ui::Widget> MakeScalarPortIcon(ui::Widget* parent);
std::unique_ptr<ui::Widget> MakeSwatchPortIcon(ui::Widget* parent);

// LeptonicaImage owns one Leptonica Pix, stored at its native depth with any
// colormap kept. GetImage() serves a cached SkImage snapshot, regenerated
// lazily after edits. Tools that find a LeptonicaImage behind their
// ImageProvider input edit the Pix directly, skipping the snapshot (see
// PhotoTool::Develop).
struct LeptonicaImage : Object {
  mutable std::mutex mutex;
  Pix* pix = nullptr;  // owned
  sk_sp<SkImage> cached_image;
  bool image_dirty = true;
  uint32_t fill_pixel = 0xffffff00;  // Leptonica RGBA word for opaque white
  mutable Str image_filename;        // ".bmp" sidecar that persists the pixels

  static constexpr int kDefaultW = 480;
  static constexpr int kDefaultH = 360;
  static constexpr int kMinSide = 16;
  static constexpr int kMaxSide = 4096;

  DEF_INTERFACE(LeptonicaImage, ImageProvider, image_provider, "Image")
  sk_sp<SkImage> GetImage() {
    auto lock = std::lock_guard(obj->mutex);
    return obj->EnsureImageLocked();
  }
  DEF_END(image_provider);

  INTERFACES(image_provider);

  LeptonicaImage();
  LeptonicaImage(const LeptonicaImage&);
  ~LeptonicaImage() override;

  StrView Name() const override { return "LeptonicaImage"; }
  Ptr<Object> Clone() const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  void EnsurePixLocked();  // creates a blank sheet if none exists
  sk_sp<SkImage> EnsureImageLocked();
  // Runs op on the owned Pix and adopts the result: in-place edit when op
  // returns the same pointer, replacement otherwise, unchanged on null.
  void Transform(const std::function<Pix*(Pix*)>&);
  void Resize(int w, int h);        // lossless canvas extend/crop
  void Dimensions(int& w, int& h);  // (0,0) if empty
};

// PhotoTool is the base class for the image-editing operations. A tool runs
// ("Develop"), chains to the next step ("Next"), and edits the image its
// autoconnecting "Image" cable points at. Subclasses supply the pixel
// operation plus presentation metadata; a shared widget draws the body,
// label, symbol and knobs. Subclass state lives in plain members, guarded by
// `mutex`.
struct PhotoTool : Object {
  mutable std::mutex mutex;  // guards `params`
  static constexpr int kMaxParams = 6;
  float params[kMaxParams] = {0, 0, 0, 0, 0, 0};

  struct ParamInfo {
    StrView name;
    float min;
    float max;
    StrView unit;                      // short suffix, e.g. "%" or ""
    bool integer = false;              // round the value to a whole number
    Span<const StrView> options = {};  // non-empty => a discrete switch over these labels
  };

  DEF_INTERFACE(PhotoTool, InterfaceArgument<ImageProvider>, image, "Image")
  static constexpr auto kStyle = Argument::Style::Cable;
  static constexpr float kAutoconnectRadius = 6_cm;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakePhotoPortIcon(p); }
  DEF_END(image);

  DEF_INTERFACE(PhotoTool, Runnable, develop, "Develop")
  void OnRun(std::unique_ptr<RunTask>&) { obj->Develop(); }
  DEF_END(develop);

  DEF_INTERFACE(PhotoTool, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(image, develop, next);

  // The pixel operation. Runs on a worker thread while the image is locked.
  // `in` is the image's Pix at its native depth; `p` is a snapshot of the
  // tool's parameters. Returns the Pix to keep: the same pointer for in-place
  // edits, a new one to replace, or nullptr to leave the image unchanged.
  virtual Pix* ApplyOp(Pix* in, const float* p) const = 0;

  // Label drawn across the body, e.g. "GRAY".
  virtual StrView Label() const = 0;
  // One sentence of plain-language help.
  virtual StrView Explanation() const { return ""; }
  // The Leptonica function the op is built on, e.g. "pixSauvolaBinarize";
  // drawn on the body. Empty = none.
  virtual StrView LeptonicaFn() const { return {}; }
  // Draws the function symbol within a 1x1cm box centred at the origin (metric, y-up).
  virtual void DrawSymbol(SkCanvas&) const {}

  // One entry per knob the tool shows.
  virtual Span<const ParamInfo> ParamInfos() const { return {}; }

  // Resolves the connected provider and applies ApplyOp. A LeptonicaImage is
  // edited in place; any other provider is read-only and reports an error.
  // Safe to call from any thread.
  void Develop();

  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  PhotoTool() = default;
  PhotoTool(const PhotoTool& o) : Object(o) {
    for (int i = 0; i < kMaxParams; ++i) params[i] = o.params[i];
  }
};

struct Threshold : PhotoTool {
  Threshold() { params[0] = 128.0f; }
  Threshold(const Threshold& o) : PhotoTool(o), bright_fg(o.bright_fg), method(o.method) {}

  // A connected number-bearing object overrides the hand-set level; the
  // control then shows the driven value as a readout.
  DEF_INTERFACE(Threshold, ObjectArgument<Object>, level_src, "Level")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakeScalarPortIcon(p); }
  DEF_END(level_src);
  INTERFACES(image, level_src, develop, next);

  bool bright_fg = false;  // polarity: which side of the cut becomes INK (false = dark side)
  int method = 0;  // 0 FIXED · 1 OTSU (global, found) · 2 SAUVOLA (local) · 3 COMPS (count-stable)

  bool DrivenLevel(float& out) const;  // true (+ the clamped value) when the port is connected
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  StrView Name() const override { return "Threshold"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Threshold, *this); }
  StrView Label() const override { return "THRESHOLD"; }
  StrView LeptonicaFn() const override {
    return method == 1   ? "pixOtsuAdaptiveThreshold"sv
           : method == 2 ? "pixSauvolaBinarizeTiled"sv
           : method == 3 ? "pixThresholdByConnComp"sv
                         : "pixThresholdToBinary"sv;
  }
  StrView Explanation() const override {
    return "Snaps the photo to pure black and white at a brightness you pick.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  void DrawSymbol(SkCanvas&) const override;
  Span<const ParamInfo> ParamInfos() const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

// Morphology: dilate/erode/open/close with an editable structuring element;
// gray tophat/dome (TOPHAT uses only the grid's size, DOME a height and no
// grid at all); hit-miss; thinning.
struct Morphology : PhotoTool {
  static constexpr int kMaxN = 9;  // grid up to 9x9
  int sel_w = 3, sel_h = 3;
  uint8_t cells[kMaxN * kMaxN] = {};  // row-major by sel_w; 0 don't-care, 1 SEL_HIT, 2 SEL_MISS
  int op_mode = 0;       // 0 dilate, 1 erode, 2 open, 3 close, 4 tophat, 5 dome, 6 hmt, 7 thin
  int height = 64;       // DOME only: minimum peak height [1..255]
  bool peaks = true;     // polarity: subject = bright stuff (true) or dark stuff (false)
  int connectivity = 8;  // THIN only: 4- or 8-connected
  bool color = false;    // binary quartet only: morph the COLORS themselves (solid brick)

  Morphology() {
    for (int i = 0; i < sel_w * sel_h; ++i) cells[i] = 1;
  }
  Morphology(const Morphology& o)
      : PhotoTool(o),
        sel_w(o.sel_w),
        sel_h(o.sel_h),
        op_mode(o.op_mode),
        height(o.height),
        peaks(o.peaks),
        connectivity(o.connectivity),
        color(o.color) {
    for (int i = 0; i < kMaxN * kMaxN; ++i) cells[i] = o.cells[i];
  }
  StrView Name() const override { return "Morphology"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Morphology, *this); }
  StrView Label() const override { return "MORPHOLOGY"; }
  StrView LeptonicaFn() const override {
    if (color && op_mode <= 3) return "pixColorMorph";
    switch (op_mode) {
      case 1:
        return "pixErode";
      case 2:
        return "pixOpen";
      case 3:
        return "pixClose";
      case 4:
        return "pixTophat";
      case 5:
        return "pixHDome";
      case 6:
        return "pixHMT";
      case 7:
        return "pixThinConnected";
      default:
        return "pixDilate";
    }
  }
  StrView Explanation() const override {
    return "Grows, shrinks, or cleans up shapes; or lifts peaks/valleys off the background.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Tone: remap brightness with a tone curve - gamma, black/white levels, invert.
struct Tone : PhotoTool {
  float gamma = 1.0f;
  int black = 0, white = 255;
  bool invert = false;

  Tone() = default;
  Tone(const Tone& o)
      : PhotoTool(o), gamma(o.gamma), black(o.black), white(o.white), invert(o.invert) {}
  StrView Name() const override { return "Tone"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Tone, *this); }
  StrView Label() const override { return "TONE"; }
  StrView LeptonicaFn() const override { return invert ? "pixInvert" : "pixGammaTRC"; }
  StrView Explanation() const override {
    return "Remaps brightness along a tone curve - gamma, black/white levels, invert.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Geometry: rotate, scale, mirror and flip.
struct Geometry : PhotoTool {
  // A connected number-bearing object overrides the hand-set angle.
  DEF_INTERFACE(Geometry, ObjectArgument<Object>, angle_src, "Angle")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakeScalarPortIcon(p); }
  DEF_END(angle_src);
  INTERFACES(image, angle_src, develop, next);

  bool DrivenAngle(float& out) const;  // true (+ clamped value) when the port is connected

  float angle_deg = 0.f;    // clockwise
  float scale = 1.0f;       // X scale (and Y while locked)
  float scale_y = 1.0f;     // Y scale (used when lock_aspect is off)
  bool absolute = false;    // SIZE mode: scale to an exact pixel target instead of a factor
  int target_w = 800;       // SIZE mode: target width px
  int target_h = 600;       // SIZE mode: target height px (derived while lock_aspect)
  bool lock_aspect = true;  // one scale for both axes
  bool pixels = false;      // chunky nearest-neighbor sampling instead of smooth scaling
  bool mirror = false;      // left-right flip (pixFlipLR)
  bool flip = false;        // top-bottom flip (pixFlipTB)

  Geometry() = default;
  Geometry(const Geometry& o)
      : PhotoTool(o),
        angle_deg(o.angle_deg),
        scale(o.scale),
        scale_y(o.scale_y),
        absolute(o.absolute),
        target_w(o.target_w),
        target_h(o.target_h),
        lock_aspect(o.lock_aspect),
        pixels(o.pixels),
        mirror(o.mirror),
        flip(o.flip) {}
  StrView Name() const override { return "Geometry"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Geometry, *this); }
  StrView Label() const override { return "GEOMETRY"; }
  StrView LeptonicaFn() const override { return absolute ? "pixScaleToSize" : "pixRotate"; }
  StrView Explanation() const override {
    return "Rotate and resize - by a factor, or to an exact pixel size.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Channel: split a colour photo into one gray channel.
struct Channel : PhotoTool {
  int channel = 0;  // 0 LUM, 1 R, 2 G, 3 B, 4 MIN, 5 MAX, 6 DIF, 7 MIX (weighted)
  float wr = 0.30f, wg = 0.59f, wb = 0.11f;  // MIX weights

  Channel() = default;
  Channel(const Channel& o) : PhotoTool(o), channel(o.channel), wr(o.wr), wg(o.wg), wb(o.wb) {}
  StrView Name() const override { return "Channel"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Channel, *this); }
  StrView Label() const override { return "CHANNEL"; }
  StrView LeptonicaFn() const override {
    return channel == 0   ? "pixConvertRGBToLuminance"
           : channel <= 3 ? "pixGetRGBComponent"
           : channel <= 6 ? "pixConvertRGBToGrayMinMax"
                          : "pixConvertRGBToGray";
  }
  StrView Explanation() const override { return "Splits a colour photo into one channel of gray."; }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Convolve: neighborhood filters. RADIUS is the neighborhood half-width in
// every mode; AMOUNT doubles as sharpening strength and bilateral tonal
// range; RANK picks which sorted neighbor wins.
struct Convolve : PhotoTool {
  // A connected number-bearing object overrides the slider.
  DEF_INTERFACE(Convolve, ObjectArgument<Object>, radius_src, "Radius")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakeScalarPortIcon(p); }
  DEF_END(radius_src);
  INTERFACES(image, radius_src, develop, next);

  bool DrivenRadius(float& out) const;

  int mode = 0;         // 0 blur, 1 sharpen, 2 edges, 3 band, 4 rank, 5 bilateral
  int radius = 3;       // half-width of the neighborhood (all modes)
  float amount = 0.5f;  // SHARP: strength [0..1]; BILAT: tonal range (maps to 5..80 gray levels)
  float rank = 0.5f;    // RANK only: which neighbor wins [0 = min, 0.5 = median, 1 = max]

  Convolve() = default;
  Convolve(const Convolve& o)
      : PhotoTool(o), mode(o.mode), radius(o.radius), amount(o.amount), rank(o.rank) {}
  StrView Name() const override { return "Convolve"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Convolve, *this); }
  StrView Label() const override { return "CONVOLVE"; }
  StrView LeptonicaFn() const override {
    return mode == 1   ? "pixUnsharpMasking"
           : mode == 2 ? "pixSobelEdgeFilter"
           : mode == 3 ? "pixHalfEdgeByBandpass"
           : mode == 4 ? "pixRankFilter"
           : mode == 5 ? "pixBilateral"
                       : "pixBlockconv";
  }
  StrView Explanation() const override {
    return "Neighborhood filters: blur, sharpen, edges, rank pick, or edge-keeping smooth.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Blend: the edited image (A) becomes blend(A, B); B comes from the second
// image input. ApplyOp resolves the provider itself.
struct Blend : PhotoTool {
  int mode = 0;         // 0 mix, 1 add, 2 multiply, 3 screen, 4 difference
  float amount = 0.5f;  // blend strength [0,1]

  DEF_INTERFACE(Blend, InterfaceArgument<ImageProvider>, paperB, "Photo B")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakePhotoPortIcon(p); }
  static constexpr float kAutoconnectRadius = 0.f;
  DEF_END(paperB);
  INTERFACES(image, paperB, develop, next);

  Blend() = default;
  Blend(const Blend& o) : PhotoTool(o), mode(o.mode), amount(o.amount) {}
  StrView Name() const override { return "Blend"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Blend, *this); }
  StrView Label() const override { return "BLEND"; }
  StrView LeptonicaFn() const override { return "pixBlend"; }
  StrView Explanation() const override {
    return "Combines two photos - mix, add, multiply, screen, or difference.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  Pix* ResolveBPix() const;  // the second image as a fresh 32-bpp Pix, or null
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Quantize: reduce to a small palette, optionally dithered.
struct Quantize : PhotoTool {
  int ncolors = 8;            // target COLOR count [2..256]
  int ngray = 4;              // MIXED only: separate gray levels [2..100]
  bool dither = false;        // Floyd-Steinberg error diffusion (median cut only)
  int algo = 0;               // 0 median cut, 1 octree, 2 mixed (color+gray), 3 from B's palette
  bool emit_palette = false;  // output the PALETTE itself as a swatch strip

  DEF_INTERFACE(Quantize, InterfaceArgument<ImageProvider>, paperB, "Palette")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakeSwatchPortIcon(p); }
  static constexpr float kAutoconnectRadius = 0.f;
  DEF_END(paperB);
  // A connected number-bearing object overrides the slider.
  DEF_INTERFACE(Quantize, ObjectArgument<Object>, ncolors_src, "Colors")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakeScalarPortIcon(p); }
  DEF_END(ncolors_src);
  INTERFACES(image, paperB, ncolors_src, develop, next);

  bool DrivenColors(float& out) const;

  Quantize() = default;
  Quantize(const Quantize& o)
      : PhotoTool(o),
        ncolors(o.ncolors),
        ngray(o.ngray),
        dither(o.dither),
        algo(o.algo),
        emit_palette(o.emit_palette) {}
  StrView Name() const override { return "Quantize"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Quantize, *this); }
  StrView Label() const override { return "QUANTIZE"; }
  StrView LeptonicaFn() const override {
    if (emit_palette) return "pixGetColormap";
    switch (algo) {
      case 1:
        return "pixOctreeQuantNumColors";
      case 2:
        return "pixMedianCutQuantMixed";
      case 3:
        return "pixOctcubeQuantFromCmap";
      default:
        return "pixMedianCutQuantGeneral";
    }
  }
  StrView Explanation() const override {
    return "Reduce to a small palette - computed, mixed colour+gray, or copied from another photo.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  Pix* ResolveBPix() const;  // the palette-source image as a fresh Pix, or null
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Flatten: even out uneven illumination by normalizing the local background.
struct Flatten : PhotoTool {
  int bgval = 200;  // target background brightness [128..240]; FLEX has no target
  int tilesize =
      10;          // illumination scale: TILE px [6..40] / MORPH close-size [3..21] / FLEX [3..10]
  int method = 0;  // estimator: 0 TILE, 1 MORPH, 2 FLEX (adaptive), 3 CONTRAST (local stretch)
  bool show_map = false;  // output the ESTIMATED ILLUMINATION MAP itself instead of the correction

  Flatten() = default;
  Flatten(const Flatten& o)
      : PhotoTool(o),
        bgval(o.bgval),
        tilesize(o.tilesize),
        method(o.method),
        show_map(o.show_map) {}
  StrView Name() const override { return "Flatten"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Flatten, *this); }
  StrView Label() const override { return "FLATTEN"; }
  StrView LeptonicaFn() const override {
    switch (method) {
      case 1:
        return show_map ? "pixGetBackgroundGrayMapMorph" : "pixBackgroundNormMorph";
      case 2:
        return "pixBackgroundNormFlex";
      case 3:
        return "pixContrastNorm";
      default:
        return show_map ? "pixGetBackgroundGrayMap" : "pixBackgroundNorm";
    }
  }
  StrView Explanation() const override {
    return "Flattens uneven lighting; can also output the estimated illumination map itself.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Posterize: snap each channel to N evenly spaced output values.
struct Posterize : PhotoTool {
  int levels = 4;  // output levels per channel [2..8]

  Posterize() = default;
  Posterize(const Posterize& o) : PhotoTool(o), levels(o.levels) {}
  StrView Name() const override { return "Posterize"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Posterize, *this); }
  StrView Label() const override { return "POSTERIZE"; }
  StrView LeptonicaFn() const override { return "pixTRCMap"; }
  StrView Explanation() const override {
    return "Reduces the photo to just a few flat colours, like a poster print.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Dither: Floyd-Steinberg error diffusion to black and white. Values within
// clip_black of 0 go solid black and within clip_white of 255 solid white,
// with no error propagated.
struct Dither : PhotoTool {
  int clip_black = 10;  // lowerclip: distance from 0 clipped to black [0..64]
  int clip_white = 10;  // upperclip: distance from 255 clipped to white [0..64]

  Dither() = default;
  Dither(const Dither& o) : PhotoTool(o), clip_black(o.clip_black), clip_white(o.clip_white) {}
  StrView Name() const override { return "Dither"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Dither, *this); }
  StrView Label() const override { return "DITHER"; }
  StrView LeptonicaFn() const override { return "pixDitherToBinarySpec"; }
  StrView Explanation() const override {
    return "Turns the photo into black-and-white dots that carry the tone, like newsprint.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Find Level: measures the photo's optimal black/white split (Otsu) and
// pushes it to the connected Number.
struct FindLevel : Object {
  mutable std::mutex mutex;
  float scorefract = 0.1f;  // valley-search width as a fraction of the max score [0..0.5]
  double last_thresh = 0, last_fg = 0, last_bg = 0;  // live measurement (toy-written)

  DEF_INTERFACE(FindLevel, InterfaceArgument<ImageProvider>, image, "Image")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakePhotoPortIcon(p); }
  DEF_END(image);

  DEF_INTERFACE(FindLevel, ObjectArgument<Object>, level, "Level")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakeScalarPortIcon(p); }
  DEF_END(level);

  DEF_INTERFACE(FindLevel, Runnable, measure, "Measure")
  void OnRun(std::unique_ptr<RunTask>&) { obj->Measure(); }
  DEF_END(measure);

  DEF_INTERFACE(FindLevel, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(image, level, measure, next);

  void Measure();            // full-res measure + push onto the connected Number
  void PushLevel(double v);  // SetText on the connected Number

  StrView Name() const override { return "Find Level"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(FindLevel, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  FindLevel() = default;
  FindLevel(const FindLevel& o) : Object(o), scorefract(o.scorefract) {}
};

// Color: hue rotation, saturation, per-channel shifts. All five params are
// fractions in [-1..1], 0 = neutral.
struct Color : PhotoTool {
  float hue = 0.f;      // pixModifyHue fraction (+-1 = +-180 deg around the wheel)
  float sat = 0.f;      // pixModifySaturation fraction
  float r_shift = 0.f;  // pixColorShiftRGB fractions
  float g_shift = 0.f;
  float b_shift = 0.f;

  Color() = default;
  Color(const Color& o)
      : PhotoTool(o),
        hue(o.hue),
        sat(o.sat),
        r_shift(o.r_shift),
        g_shift(o.g_shift),
        b_shift(o.b_shift) {}
  StrView Name() const override { return "Color"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Color, *this); }
  StrView Label() const override { return "COLOR"; }
  StrView LeptonicaFn() const override {
    if (hue != 0.f) return "pixModifyHue";
    if (sat != 0.f) return "pixModifySaturation";
    if (r_shift != 0.f || g_shift != 0.f || b_shift != 0.f) return "pixColorShiftRGB";
    return "pixModifyHue";
  }
  StrView Explanation() const override {
    return "Spins the colours around the wheel, boosts or drains them, trims each channel.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Warp: stretch one edge, shear quadratically, or ripple with harmonic waves.
struct Warp : PhotoTool {
  int mode = 2;         // 0 STRETCH (horizontal), 1 SHEAR (vertical), 2 WAVES (harmonic)
  float amount = 0.5f;  // [0..1] strength

  Warp() = default;
  Warp(const Warp& o) : PhotoTool(o), mode(o.mode), amount(o.amount) {}
  StrView Name() const override { return "Warp"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Warp, *this); }
  StrView Label() const override { return "WARP"; }
  StrView LeptonicaFn() const override {
    return mode == 0   ? "pixStretchHorizontal"
           : mode == 1 ? "pixQuadraticVShear"
                       : "pixRandomHarmonicWarp";
  }
  StrView Explanation() const override {
    return "Bends the page - stretches an edge, leans it over, or ripples it like water.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Measure: reads a statistic (mean/min/max brightness) off a region of the
// photo and pushes it to the connected Number.
struct Measure : Object {
  mutable std::mutex mutex;
  float u0 = 0.25f, v0 = 0.25f, u1 = 0.75f, v1 = 0.75f;  // the measured region, normalized
  int stat = 0;                                          // 0 MEAN, 1 MIN, 2 MAX
  int last_value = 0;                                    // live measurement (toy-written)

  DEF_INTERFACE(Measure, InterfaceArgument<ImageProvider>, image, "Image")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakePhotoPortIcon(p); }
  DEF_END(image);

  DEF_INTERFACE(Measure, ObjectArgument<Object>, value, "Value")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakeScalarPortIcon(p); }
  DEF_END(value);

  DEF_INTERFACE(Measure, Runnable, measure, "Measure")
  void OnRun(std::unique_ptr<RunTask>&) { obj->DoMeasure(); }
  DEF_END(measure);

  DEF_INTERFACE(Measure, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(image, value, measure, next);

  void DoMeasure();       // full-res measure + push
  void PushValue(int v);  // SetText on the connected Number

  StrView Name() const override { return "Measure"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Measure, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  Measure() = default;
  Measure(const Measure& o) : Object(o), u0(o.u0), v0(o.v0), u1(o.u1), v1(o.v1), stat(o.stat) {}
};

// Select: emit a 1-bpp mask of the pixels whose value falls inside (or
// outside) a band.
struct Select : PhotoTool {
  int axes = 0;            // 0 LUM band; or a 2-D HSV band: 1 H*S, 2 H*V, 3 S*V
  int lo = 80, hi = 180;   // axis-A band (LUM/SAT 0..255; HUE 0..239)
  int lo2 = 0, hi2 = 255;  // axis-B band [0..255]; only the 2-D modes use it
  bool inside = true;      // keep in-band (true) or out-of-band

  Select() = default;
  Select(const Select& o)
      : PhotoTool(o), axes(o.axes), lo(o.lo), hi(o.hi), lo2(o.lo2), hi2(o.hi2), inside(o.inside) {}
  StrView Name() const override { return "Select"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Select, *this); }
  StrView Label() const override { return "SELECT"; }
  StrView LeptonicaFn() const override {
    switch (axes) {
      case 1:
        return "pixMakeRangeMaskHS";
      case 2:
        return "pixMakeRangeMaskHV";
      case 3:
        return "pixMakeRangeMaskSV";
      default:
        return "pixGenerateMaskByBand";
    }
  }
  StrView Explanation() const override {
    return "Picks out the pixels inside the band you bracket - by brightness, or a 2-D HSV band.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Fade: fade one edge of the photo toward white or black.
struct Fade : PhotoTool {
  int dir = 0;            // 0 left, 1 right, 2 top, 3 bottom
  bool to_black = false;  // fade target: white (default) or black
  float reach = 0.4f;     // fraction of the image the ramp covers [0..1]
  float strength = 0.9f;  // max fade at the edge [0..1]

  Fade() = default;
  Fade(const Fade& o)
      : PhotoTool(o), dir(o.dir), to_black(o.to_black), reach(o.reach), strength(o.strength) {}
  StrView Name() const override { return "Fade"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Fade, *this); }
  StrView Label() const override { return "FADE"; }
  StrView LeptonicaFn() const override { return "pixLinearEdgeFade"; }
  StrView Explanation() const override {
    return "Dissolves one edge of the photo toward white or black.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Reduce: shrink by an integer factor; the rule picks which local statistic
// survives.
struct Reduce : PhotoTool {
  int factor_idx = 0;  // 0..3 -> x2, x3, x4, x8 (RANK supports 2/4/8 only)
  int rule = 0;        // 0 GRAY, 1 MIN, 2 MAX, 3 DIFF, 4 RANK
  int rank = 2;        // RANK only [1..4]

  Reduce() = default;
  Reduce(const Reduce& o) : PhotoTool(o), factor_idx(o.factor_idx), rule(o.rule), rank(o.rank) {}
  StrView Name() const override { return "Reduce"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Reduce, *this); }
  StrView Label() const override { return "REDUCE"; }
  StrView LeptonicaFn() const override {
    switch (rule) {
      case 1:
      case 2:
      case 3:
        return "pixScaleGrayMinMax";
      case 4:
        return "pixScaleGrayRank2";
      default:
        return "pixScaleToGray";
    }
  }
  StrView Explanation() const override {
    return "Shrinks by an integer factor; the rule picks which local detail survives.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Count: counts connected components of the binarized image and pushes the
// number to the connected Number. Under 8-connectivity diagonal touches join
// shapes, so the chooser changes the answer.
struct Count : Object {
  mutable std::mutex mutex;
  bool eight = true;   // 8-connectivity: diagonal touches join shapes
  int last_count = 0;  // live measurement (toy-written)

  DEF_INTERFACE(Count, InterfaceArgument<ImageProvider>, image, "Image")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakePhotoPortIcon(p); }
  DEF_END(image);

  DEF_INTERFACE(Count, ObjectArgument<Object>, count, "Count")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakeScalarPortIcon(p); }
  DEF_END(count);

  DEF_INTERFACE(Count, Runnable, measure, "Measure")
  void OnRun(std::unique_ptr<RunTask>&) { obj->Measure(); }
  DEF_END(measure);

  DEF_INTERFACE(Count, NextArg, next, "Next")
  DEF_END(next);

  INTERFACES(image, count, measure, next);

  void Measure();         // full-res count + push onto the connected Number
  void PushCount(int n);  // SetText on the connected Number

  StrView Name() const override { return "Count"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Count, *this); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;

  Count() = default;
  Count(const Count& o) : Object(o), eight(o.eight) {}
};

// Seedfill: flood the connected component under the seed pin. The input
// binarized at 130 is the mask; a 3x3 block at the pin is the seed.
struct Seedfill : PhotoTool {
  float seed_u = 0.5f;            // seed point, normalized [0..1] across the image width
  float seed_v = 0.5f;            // ... and height (0 = top)
  bool eight = true;              // 8-connectivity (diagonals count) vs 4
  uint32_t paint_rgb = 0xED1C24;  // the poured paint (MS-Paint red by default)
  bool emit_mask = false;         // true = output the 1-bpp component instead of painting

  Seedfill() = default;
  Seedfill(const Seedfill& o)
      : PhotoTool(o),
        seed_u(o.seed_u),
        seed_v(o.seed_v),
        eight(o.eight),
        paint_rgb(o.paint_rgb),
        emit_mask(o.emit_mask) {}
  StrView Name() const override { return "Seedfill"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Seedfill, *this); }
  StrView Label() const override { return "SEEDFILL"; }
  StrView LeptonicaFn() const override { return "pixSeedfillBinary"; }
  StrView Explanation() const override {
    return "Drops a pin on the page and floods the connected shape under it.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Crop: keep only the box (u0,v0)-(u1,v1), normalized, v = 0 at the top.
// Legacy x_min/x_max/y_min/y_max state keys map onto the box, y flipped.
struct CropRegion : PhotoTool {
  float u0 = 0.15f, v0 = 0.15f;  // top-left of the keep-box, normalized
  float u1 = 0.85f, v1 = 0.85f;  // bottom-right

  CropRegion() = default;
  CropRegion(const CropRegion& o) : PhotoTool(o), u0(o.u0), v0(o.v0), u1(o.u1), v1(o.v1) {}
  StrView Name() const override { return "Crop"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(CropRegion, *this); }
  StrView Label() const override { return "CROP"; }
  StrView LeptonicaFn() const override { return "pixClipRectangle"; }
  StrView Explanation() const override {
    return "Cuts out just the part of the photo inside the frame.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Deskew: measure the page's skew and rotate it straight. UPRIGHT sweeps
// +-7 deg; ANY also checks the 90-deg orientation. Below confidence 3.0 the
// input is returned unchanged.
struct Deskew : PhotoTool {
  int mode = 0;  // 0 = upright, 1 = any orientation

  // Output port: a connected Number receives the measured correction angle.
  DEF_INTERFACE(Deskew, ObjectArgument<Object>, fix_out, "Fix")
  static constexpr auto kStyle = Argument::Style::Cable;
  std::unique_ptr<ui::Widget> OnMakeIcon(ui::Widget* p) { return MakeScalarPortIcon(p); }
  DEF_END(fix_out);
  INTERFACES(image, fix_out, develop, next);

  void PushFix(double deg);  // -> the connected Number's text

  Deskew() = default;
  Deskew(const Deskew& o) : PhotoTool(o), mode(o.mode) {}
  StrView Name() const override { return "Deskew"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Deskew, *this); }
  StrView Label() const override { return "DESKEW"; }
  StrView LeptonicaFn() const override {
    return mode == 1 ? "pixFindSkewOrthogonalRange" : "pixFindSkewAndDeskew";
  }
  StrView Explanation() const override {
    return "Measures how crooked the page is and rotates it straight.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// Generate: fill the paper with generated content - a colour chart or noise.
// (No TEXT mode: leptonica's bitmap fonts are TIFF-encoded and every image
// codec is disabled in this build.)
struct Generate : PhotoTool {
  int mode = 0;    // 0 gamut, 1 noise
  int scale = 2;   // GAMUT: pixMakeGamutRGB scale [1..4]
  int stdev = 20;  // NOISE: gaussian standard deviation [2..80]

  Generate() = default;
  Generate(const Generate& o) : PhotoTool(o), mode(o.mode), scale(o.scale), stdev(o.stdev) {}
  StrView Name() const override { return "Generate"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(Generate, *this); }
  StrView Label() const override { return "GENERATE"; }
  StrView LeptonicaFn() const override {
    return mode == 1 ? "pixAddGaussianNoise" : "pixMakeGamutRGB";
  }
  StrView Explanation() const override {
    return "Fills the page with generated content: a colour chart or noise.";
  }
  Pix* ApplyOp(Pix* in, const float* p) const override;
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
  void SerializeState(ObjectSerializer&) const override;
  bool DeserializeKey(ObjectDeserializer&, StrView key) override;
};

// LeptonicaShelf: the curated, grouped set of tools. Dragging a tool off the
// shelf drops a working copy onto the board.
struct LeptonicaShelf : Object {
  StrView Name() const override { return "Leptonica"; }
  Ptr<Object> Clone() const override { return MAKE_PTR(LeptonicaShelf); }
  std::unique_ptr<ObjectToy> MakeToy(ui::Widget* parent) override;
};

}  // namespace automat::library
