#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

// Warning: coded with a stochastic parrot

// Controls for the Leptonica image-processing objects. Each control stands for
// one recurring operation parameter — a threshold on the intensity axis, a
// structuring-element grid, a mode selector — so the same control appears
// wherever that parameter does.
//
// Everything draws in pixel coordinates with +Y down, inside bounds the caller
// passes in. The functions are stateless: the hosting widget owns the values.
// Declarations here; bodies in ui_leptonica.cpp.

#include <include/core/SkColor.h>
#include <include/core/SkPoint.h>
#include <include/core/SkRect.h>

#include <cstdint>

#include "ui_beta.hpp"

class SkCanvas;
class SkPath;

namespace automat::ui::leptonica {

// Level: picks one value in [vmin,vmax] on a horizontal band drawn over the
// image's brightness histogram. `histogram` is 256 bins or null;
// `max_log_count` is the precomputed max of log(count+1) over the bins. The
// marker knob extends above `band` and a grayscale ramp below it, so leave
// room around the rectangle. With `show_comparator`, a flag on the knob points
// at the kept side (`keep_above`).
float LevelValueToX(const SkRect& band, float value, float vmin, float vmax);
float LevelXToValue(const SkRect& band, float x, float vmin, float vmax);
bool LevelGrabsMarker(const SkRect& band, SkPoint p, float value, float vmin, float vmax,
                      float grip = 0.f);
void DrawLevel(SkCanvas& canvas, const SkRect& band, float value, float vmin, float vmax,
               const uint32_t* histogram, float max_log_count, bool keep_above,
               bool show_comparator, beta::State state, uint32_t seed);

// Window: the two-marker variant of Level. lo..hi bracket the active range;
// values outside it are clipped. Same band layout as Level; hit-test the
// markers with LevelGrabsMarker and map values with LevelValueToX/XToValue.
void DrawWindow(SkCanvas& canvas, const SkRect& band, float lo, float hi, float vmin, float vmax,
                const uint32_t* histogram, float max_log_count, beta::State state_lo,
                beta::State state_hi, uint32_t seed);

// Connectivity: chooses 4- or 8-connectivity. Two cells showing the neighbor
// directions as spokes.
SkRect ConnectivityCell(const SkRect& r, int which);  // which: 0 = 4-conn, 1 = 8-conn
int ConnectivityHit(const SkRect& r, SkPoint p);      // 0 = miss, else 4 or 8
void DrawConnectivity(SkCanvas& canvas, const SkRect& r, bool eight, beta::State state,
                      uint32_t seed);

// Region: a rectangular selection dragged on the image preview. `fit` is the
// displayed image's rect, `mq` the selection inside it; the area outside the
// selection is dimmed.
int RegionHit(const SkRect& mq, SkPoint p,
              float grip = 10.f);  // 1..4 = TL/TR/BL/BR grip, 5 = inside, 0 = miss
void DrawRegion(SkCanvas& canvas, const SkRect& fit, const SkRect& mq, beta::State state,
                uint32_t seed);

// Transform ring: an angle handle drawn around the preview — a ring with a
// knob at `angle_deg` (0 = up, clockwise). Drag anywhere on the ring band.
bool TransformRingHit(SkPoint c, float radius, SkPoint p, float band = 12.f);
float TransformRingAngleAt(SkPoint c, SkPoint p);  // degrees, 0 = up, clockwise
void DrawTransformRing(SkCanvas& canvas, SkPoint c, float radius, float angle_deg,
                       beta::State state, uint32_t seed);

// Polarity: a two-ended switch for which value counts as foreground — one end
// dark, one light, the active end ringed. Click anywhere on it to flip.
bool PolarityHit(const SkRect& r, SkPoint p);
void DrawPolarity(SkCanvas& canvas, const SkRect& r, bool bright_hot, beta::State state,
                  uint32_t seed);

// Palette: a 2x5 strip of color swatches; the selected cell is ringed.
constexpr SkColor kPaletteColors[] = {
    0xff1a1a1a,  // ink
    0xffffffff,  // white
    0xffed1c24,  // red
    0xffff7f27,  // orange
    0xfffff200,  // yellow
    0xff22b14c,  // green
    0xff00a2e8,  // cyan
    0xff3f48cc,  // blue
    0xffa349a4,  // purple
    0xffff7bac,  // rose
};
constexpr int kPaletteCount = 10;

SkRect PaletteCell(const SkRect& r, int i);
int PaletteHit(const SkRect& r, SkPoint p);  // -1 = miss, else the palette index
void DrawPalette(SkCanvas& canvas, const SkRect& r, int selected, beta::State state, uint32_t seed);

// Depth chip: a read-only badge stating an image's depth in bits per pixel
// (glyph + numeral; a colormap shows as a small palette strip).
void DrawDepthChip(SkCanvas& canvas, const SkRect& r, int depth, bool has_cmap, beta::State state,
                   uint32_t seed);

// Stamp: an editable w*h grid for a structuring element or kernel. `cells` is
// row-major; each cell holds 0 = don't care, 1 = hit, 2 = miss. (ox,oy) marks
// the origin cell. The presets overwrite `cells` with full / cross / disk
// patterns.
void StampGridMetrics(const SkRect& rect, int w, int h, float& cell, float& gx, float& gy);
bool StampCellAt(const SkRect& rect, SkPoint p, int w, int h, int& cx, int& cy);
void StampPresetBrick(uint8_t* cells, int w, int h);
void StampPresetCross(uint8_t* cells, int w, int h);
void StampPresetDisk(uint8_t* cells, int w, int h);
void DrawStamp(SkCanvas& canvas, const SkRect& rect, const uint8_t* cells, int w, int h, int ox,
               int oy, beta::State state, uint32_t seed);

// Mode wheel: selects one of n labelled options arranged around a dial; every
// option stays visible. `glyphs`, if given, is an array of n paths; the
// selected option's glyph is drawn on the dial face. Glyph paths are authored
// in a unit box (x in [-1.4, 1.4], y in [-0.6, 0.6], +Y down).
float ModeWheelAngle(int k, int n);
int ModeWheelHit(SkPoint c, float radius, SkPoint p, int n);  // nearest option, or -1 outside
void DrawModeWheel(SkCanvas& canvas, SkPoint c, float radius, const char* const* labels, int n,
                   int selected, beta::State state, uint32_t seed, const SkPath* glyphs = nullptr);

// Curve: draws a 256-entry tone curve `lut` — input 0..255 left to right,
// output 0..255 bottom to top — over a grid with the identity diagonal as
// reference.
void DrawCurveLUT(SkCanvas& canvas, const SkRect& rect, const uint8_t* lut, beta::State state,
                  uint32_t seed);

// Dial: an angle knob with degree ticks. 0 degrees points up, increasing
// clockwise; angle_deg in [0,360).
float DialAngleAt(SkPoint c, SkPoint p);
void DrawDial(SkCanvas& canvas, SkPoint c, float radius, float angle_deg, beta::State state,
              uint32_t seed);

// Channel tap: a row of n buttons selecting a color channel. The default four
// are LUM/R/G/B; `labels` and `colors` override them.
int ChannelTapAt(const SkRect& r, SkPoint p, int n);  // -1 = miss, else the tap index
void DrawChannelTap(SkCanvas& canvas, const SkRect& r, int selected, beta::State state,
                    uint32_t seed, const char* const* labels = nullptr,
                    const SkColor* colors = nullptr, int n = 4);

}  // namespace automat::ui::leptonica
