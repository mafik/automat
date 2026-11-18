// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_memory.hpp"

#include <include/core/SkAlphaType.h>
#include <include/core/SkColor.h>
#include <include/core/SkColorType.h>
#include <include/core/SkImage.h>
#include <include/core/SkImageInfo.h>
#include <include/core/SkMatrix.h>
#include <include/core/SkPaint.h>
#include <include/core/SkPath.h>
#include <include/core/SkPathBuilder.h>
#include <include/core/SkShader.h>
#include <include/core/SkTileMode.h>
#include <include/effects/SkGradient.h>
#include <modules/svg/include/SkSVGDOM.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdint>

#include "animation.hpp"
#include "argument.hpp"
#include "audio.hpp"
#include "board.hpp"
#include "color.hpp"
#include "embedded.hpp"
#include "fd.hpp"
#include "font.hpp"
#include "format.hpp"
#include "keyboard.hpp"
#include "library_key_presser.hpp"
#include "library_mouse.hpp"
#include "library_timeline.hpp"
#include "log.hpp"
#include "math.hpp"
#include "mem_meld.hpp"
#include "memory_landmarks.hpp"
#include "object.hpp"
#include "root_widget.hpp"
#include "run_button.hpp"
#include "sincos.hpp"
#include "span.hpp"
#include "status.hpp"
#include "svg.hpp"
#include "textures.hpp"
#include "time.hpp"
#include "ui_button.hpp"
#include "ui_connection_widget.hpp"
#include "ui_constants.hpp"
#include "ui_shape_widget.hpp"
#include "widget.hpp"

namespace automat {
Str ToStr(const MemMeld::Region& reg) {
  return f("{}@{}[{}{}{}]+{} at {}", reg.path.empty() ? "Region"sv : reg.path, (void*)reg.remote,
           reg.prot & PROT_READ ? 'R' : '-', reg.prot & PROT_WRITE ? 'W' : '-',
           reg.prot & PROT_EXEC ? 'X' : '-', reg.Size(), (void*)reg.local.data());
}
}  // namespace automat

namespace automat::library {

// Idea: More Objects
// 1. Can map memory of another process (already present!)
// 2. Can select a sub-section of this memory (preserves map information)
// 3. Scans memory for interesting regions
// Opens possibilities of extra composable objects:
// 1b. Core dump
// 1c. Local memory
// 1d. ELF file
// 1e. Regular ptrace-based attach
// 2b. Selects some file
// 2c. Follows some pointer + size
// 3b. Interprets memory as a specific data type

// How Memory object will work:
// - will expose a "Refreshed" notification / command - (no-op for MemMeld) that notifies that
// memory contents have been refreshed
// - will show just the memory ranges as a simple color-coded line
// - will allow dropping for "Crop" objects

// "Crop" object:
// - can be dropped on another toy to select a sub-range of it
//
// Problem: "Crop" object needs to ask the underlying "Memory" about the range described by some
// rect. Shape of objects is NOT part of the VM - this must happen in the UI. So Toys need to be
// able to talk with each other. So the "croppable" interface must be defined on the Toy.
//
// Design A: reuse the drag&drop infrastructure and create a "Crop" object that can be "dropped
//                 into" other objects and then moved around to select their sub-sections. Clones of
//                 the Crop object will create a toy for the original object parametrized by the
//                 cropped section. This way the "Crop" object doesn't need to care about the type
//                 of the object it's dropped onto. The "embedded" clone of Crop will be positioned
//                 by the parent toy so if position is changed, it'll be re-positioned during Tick
//
// Whet the "Crop" is dropped onto Memory, Memory will tell it how many control points to create and
// they'll be stored as child objects within memory.
//
// The original Crop will immediately create a "cropped" version of the toy it was dropped into.
//
// Effectively the "crop target" is going to be the first owner that implements "croppable".
//
// Process Object Idea:
// - Object that represents arbitrary process in the OS
// - uses pidfd to track some OS process
// - shows PID as text
// - shows the "mind meld" symbol (Star Trek reference)

// When the arrow is dragged out it'll create a MemMeld object.
// - allows dropping & interaction with the Crop object
// - it could be a snow globe
//
// The plan:
// - Process object (snow globe) will be available in the bubble menu of Window
// -

using namespace automat::ui;
using namespace std;

MemMeld::MemMeld() {}

void MemMeld::Start() {
  constexpr int pid = 948318;
  FD pidfd = syscall(SYS_pidfd_open, pid, 0);
  if (!pidfd.Opened()) {
    Status status;
    AppendErrorMessage(status) += "Couldn't open pidfd";
    ERROR << status;
    return;
  }

  Status status;
  mem_meld.on_attached = [&]() { WakeToys(); };
  mem_meld.on_changed = [&](const automat::MemMeld::Region& before,
                            const automat::MemMeld::Region& after) { WakeToys(); };
  mem_meld.on_mapped = [&](const automat::MemMeld::Region& region) { WakeToys(); };
  mem_meld.Attach(pidfd, status);
  WakeToys();
  if (!OK(status)) {
    ERROR << status;
    return;
  }
}

struct MemMeldToy : ObjectToy {
  ui::Button btn_attach;

  MemMeldToy(Widget* parent, MemMeld& mv) : ObjectToy(parent, mv), btn_attach(this) {
    btn_attach.child = MakeShapeWidget(&btn_attach, PathFromSVG(kPlayShape), "#f56a00"_color);
    btn_attach.UpdateChildTransform();
    btn_attach.target = mv.start.Bind();
    btn_attach.local_to_parent = SkM44(SkMatrix::Translate(-5_cm, -5_cm));
  }

  constexpr static Rect kShapeRect = Rect::MakeCenterZero(10_cm, 15_cm);
  SkPath Shape() const override { return SkPath::Rect(kShapeRect); }

  Vec<automat::MemMeld::Region> regions;
  Vec<AddressRange> address_ranges;  // merged address ranges in the remote process
  uint64_t n_bytes = 0;
  Vec<sk_sp<SkImage>> images;

  Tock Tick(time::Timer&) override {
    auto mv = LockObject<MemMeld>();
    auto& m = mv->mem_meld;

    regions.clear();
    address_ranges.clear();
    n_bytes = 0;

    {  // Refresh `regions`
      automat::MemMeld::ReadGuard guard(m);
      // TODO: rather than re-querying the tree, introduce an API to walk the tree
      for (auto* it = guard.FindLowerBound(0); it != nullptr;
           it = guard.FindLowerBound(it->remote + it->Size())) {
        regions.Append(*it);
        n_bytes += it->Size();
      }
    }
    if (regions.empty()) return Tock::Draw;  // nothing to animate

    {  // Update `address_ranges`
      address_ranges.emplace_back(regions.front().remote,
                                  regions.front().remote + regions.front().Size());
      for (int i = 1; i < regions.size(); ++i) {
        auto& r = regions[i];
        if (address_ranges.back().end == r.remote) {  // merge
          address_ranges.back().end = r.remote + r.Size();
        } else {
          address_ranges.emplace_back(r.remote, r.remote + r.Size());
        }
      }
    }

    images.clear();
    for (auto current_region = regions.begin(); current_region != regions.end(); ++current_region) {
      auto current = current_region->local;
      int pixels = current_region->Size() / 4;
      if (pixels > 2000) continue;
      int width = std::max(8, ((int)sqrt(pixels)) & ~7);
      int height = pixels / width;
      if (width > 0 && height > 0) {
        auto info =
            SkImageInfo::Make(SkISize(width, height), kRGB_888x_SkColorType, kOpaque_SkAlphaType);
        // auto info = SkImageInfo::MakeN32(width, height, kUnpremul_SkAlphaType);
        auto pixmap = SkPixmap(info, current.data(), width * 4);
        images.emplace_back(SkImages::RasterFromPixmap(pixmap, nullptr, nullptr));
      }
    }
    return Tock::Drawing;
  }

  void Draw(SkCanvas& canvas) const override {
    auto mv = LockObject<MemMeld>();
    SkPaint bg;
    bg.setColor("#e3c2a8"_color);
    canvas.drawPath(shape, bg);

    GetFont().DrawText(canvas, f("size: {}", mv->mem_meld.Local().size_bytes()), {});

    Rect zone_contents = kShapeRect;
    Rect zone_spear = zone_contents.CutRight(4_mm);

    int side = floor(sqrt(images.size()));
    if (side > 0) {
      float cell_width = zone_contents.Width() / side;
      float cell_height = zone_contents.Height() / side;
      for (int i = 0; i < images.size(); ++i) {
        auto& img = images[i];
        int column = i % side;
        int row = i / side;
        float left = zone_contents.left + column * cell_width;
        float right = left + cell_width;
        float bottom = zone_contents.bottom + row * cell_height;
        float top = bottom + cell_height;
        Rect cell = Rect(left, bottom, right, top);
        // canvas.drawImageRect(img.get(), cell, kFastSamplingOptions);
      }

      {
        int n_gaps = address_ranges.size() - 1;
        if (address_ranges.front().begin > 0) n_gaps += 1;
        if (address_ranges.back().end < UINT64_MAX) n_gaps += 1;

        constexpr float kZigZagW = 0.5_mm;
        // Zig-zag bends at 60 degrees to the right (W to the right, W/2 up), then at 60 degrees
        // left (2W to the left, W up), then back 60deg to the right (W to the right, ending in the
        // middle, W/2 up). Total height is W*2.

        float shaft_height = zone_spear.Height() - 6_mm;
        float gap_height = min(kZigZagW * 2, shaft_height * 0.5f / n_gaps);
        float gaps_height = gap_height * n_gaps;
        float bytes_height = shaft_height - gaps_height;

        {
          SkPaint shaft_paint;
          SkPathBuilder shaft_builder;
          float x = zone_spear.CenterX();
          float y = zone_spear.bottom;
          uintptr_t addr = 0;
          constexpr float kW = kMargin / 2;
          shaft_builder.moveTo(zone_spear.BottomCenter() + Vec2(kW, 0));
          for (auto [a, b] : address_ranges) {
            if (a > addr) {
              y += gap_height / 4;
              shaft_builder.lineTo(x + kW + kZigZagW, y);
              y += gap_height / 2;
              shaft_builder.lineTo(x + kW - kZigZagW, y);
              y += gap_height / 4;
              shaft_builder.lineTo(x + kW, y);
            }
            // float bottom = y;
            y += bytes_height * (b - a) / n_bytes;
            shaft_builder.lineTo(x + kW, y);
            addr = b;
            // float top = y;
            // canvas.drawRect(Rect(x - kW, bottom, x + kW, top), {});
          }
          if (UINT64_MAX > addr) {
            y += gap_height / 4;
            shaft_builder.lineTo(x + kW + kZigZagW, y);
            y += gap_height / 2;
            shaft_builder.lineTo(x + kW - kZigZagW, y);
            y += gap_height / 4;
            shaft_builder.lineTo(x + kW, y);
          }
          addr = UINT64_MAX;
          shaft_builder.lineTo(zone_spear.right, zone_spear.top - 5_mm);
          shaft_builder.lineTo(zone_spear.TopCenter());
          shaft_builder.lineTo(zone_spear.left, zone_spear.top - 5_mm);
          shaft_builder.lineTo(x - kW, y);
          for (auto [a, b] : std::ranges::reverse_view(address_ranges)) {
            if (b < addr) {
              y -= gap_height / 4;
              shaft_builder.lineTo(x - kW - kZigZagW, y);
              y -= gap_height / 2;
              shaft_builder.lineTo(x - kW + kZigZagW, y);
              y -= gap_height / 4;
              shaft_builder.lineTo(x - kW, y);
            }
            y -= bytes_height * (b - a) / n_bytes;
            shaft_builder.lineTo(x - kW, y);
            addr = a;
          }
          if (addr > 0) {
            y -= gap_height / 4;
            shaft_builder.lineTo(x - kW - kZigZagW, y);
            y -= gap_height / 2;
            shaft_builder.lineTo(x - kW + kZigZagW, y);
            y -= gap_height / 4;
            shaft_builder.lineTo(x - kW, y);
          }
          shaft_builder.close();

          canvas.drawPath(shaft_builder.detach(), shaft_paint);
        }
      }
    }

    return BakeChildren(canvas);
  }
};

std::unique_ptr<MemMeld::Toy> MemMeld::MakeToy(ui::Widget* parent) {
  return make_unique<MemMeldToy>(parent, *this);
}

}  // namespace automat::library
