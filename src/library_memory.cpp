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

using namespace automat::ui;
using namespace std;

Memory::Memory() {}

void Memory::Start() {
  LOG << "Start";
  constexpr int pid = 948318;
  FD pidfd = syscall(SYS_pidfd_open, pid, 0);
  if (!pidfd.Opened()) {
    Status status;
    AppendErrorMessage(status) += "Couldn't open pidfd";
    ERROR << status;
    return;
  }

  LOG << "Pidfd=" << pidfd;
  Status status;
  mem_meld.on_attached = [&]() {
    LOG << "on_attached";
    WakeToys();
  };
  mem_meld.on_changed = [&](const MemMeld::Region& before, const MemMeld::Region& after) {
    LOG << "on_changed";
    if (before.remote == after.remote)
      LOG << "  @" << f("{}", (void*)before.remote);
    else
      LOG << "  @" << f("{}", (void*)before.remote) << " => @" << f("{}", (void*)after.remote);
    // if (before.)
    if (!before.path.empty()) LOG << "  path [" << before.path << "] => [" << after.path << "]";
    last_region = after;
    WakeToys();
  };
  mem_meld.on_forked = [](int pid) { LOG << "on_forked " << pid; };
  mem_meld.on_mapped = [&](const MemMeld::Region& region) {
    LOG << "on_mapped " << region;
    last_region = region;
    WakeToys();
  };
  mem_meld.Attach(pidfd, status);
  LOG << "Attach exited";
  WakeToys();
  if (!OK(status)) {
    ERROR << status;
    return;
  }
}

struct MemoryToy : ObjectToy {
  ui::Button btn_attach;

  MemoryToy(Widget* parent, Memory& mv) : ObjectToy(parent, mv), btn_attach(this) {
    btn_attach.child = MakeShapeWidget(&btn_attach, PathFromSVG(kPlayShape), "#f56a00"_color);
    btn_attach.UpdateChildTransform();
    btn_attach.target = mv.start.Bind();
    btn_attach.local_to_parent = SkM44(SkMatrix::Translate(-5_cm, -5_cm));
  }

  constexpr static Rect kShapeRect = Rect::MakeCenterZero(10_cm, 10_cm);
  SkPath Shape() const override { return SkPath::Rect(kShapeRect); }

  Vec<MemMeld::Region> regions;
  Vec<sk_sp<SkImage>> images;

  Tock Tick(time::Timer&) override {
    auto mv = LockObject<Memory>();
    auto& m = mv->mem_meld;

    {  // Refresh `regions`
      regions.clear();
      MemMeld::ReadGuard guard(m);
      // TODO: rather than re-querying the tree, introduce an API to walk the tree
      for (auto* it = guard.FindLowerBound(0); it != nullptr;
           it = guard.FindLowerBound(it->remote + it->Size())) {
        regions.Append(*it);
      }
    }
    if (regions.empty()) return Tock::Draw;  // nothing to animate

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
    auto mv = LockObject<Memory>();
    SkPaint bg;
    bg.setColor("#e3c2a8"_color);
    canvas.drawPath(shape, bg);

    GetFont().DrawText(canvas, f("size: {}", mv->mem_meld.Local().size_bytes()), {});

    Rect zone_contents = kShapeRect;
    Rect zone_spear = zone_contents.CutRight(1_cm);

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
        canvas.drawImageRect(img.get(), cell, kFastSamplingOptions);
      }

      {
        int n_gaps = 0;
        int64_t n_bytes = regions.empty() ? 0 : regions.front().Size();
        for (int i = 1; i < regions.size(); ++i) {
          auto& prev = regions[i - 1];
          auto& curr = regions[i];
          if (prev.remote + prev.Size() < curr.remote) ++n_gaps;
          n_bytes += curr.Size();
        }
        constexpr float kZigZagW = 2_mm;
        // Zig-zag bends at 60 degrees to the right (W to the right, W/2 up), then at 60 degrees
        // left (2W to the left, W up), then back 60deg to the right (W to the right, ending in the
        // middle, W/2 up). Total height is W*2.

        float shaft_height = zone_spear.Height() - 1_cm;
        float gap_height = min(kZigZagW * 2, shaft_height * 0.2f / n_gaps);
        float gaps_height = gap_height * n_gaps;
        float bytes_height = shaft_height - gaps_height;

        SkPaint shaft_paint;
        shaft_paint.setStyle(SkPaint::kStroke_Style);
        shaft_paint.setStrokeWidth(kMargin);
        SkPathBuilder shaft_builder;
        shaft_builder.moveTo(zone_spear.BottomCenter());
        float x = zone_spear.CenterX();
        float y = zone_spear.bottom;
        uintptr_t addr = 0;
        for (auto& r : regions) {
          if (r.remote > addr) {
            y += gap_height / 4;
            shaft_builder.lineTo(x + kZigZagW, y);
            y += gap_height / 2;
            shaft_builder.lineTo(x - kZigZagW, y);
            y += gap_height / 4;
            shaft_builder.lineTo(x, y);
          }
          y += bytes_height * r.Size() / n_bytes;
          shaft_builder.lineTo(x, y);
          addr = r.remote + r.Size();
        }

        canvas.drawPath(shaft_builder.detach(), shaft_paint);
      }
    }

    return BakeChildren(canvas);
  }
};

std::unique_ptr<Memory::Toy> Memory::MakeToy(ui::Widget* parent) {
  return make_unique<MemoryToy>(parent, *this);
}

}  // namespace automat::library
