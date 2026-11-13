// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "library_memory_viewer.hpp"

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
#include "llvm/Support/MathExtras.h"
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
           reg.prot & PROT_EXEC ? 'X' : '-', reg.local.size_bytes(), (void*)reg.local.data());
}
}  // namespace automat

namespace automat::library {

using namespace automat::ui;
using namespace std;

MemoryViewer::MemoryViewer() {}

void MemoryViewer::Start() {
  LOG << "Start";
  constexpr int pid = 2608465;
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
  mem_meld.on_detached = [&](const Status& status) {
    if (OK(status)) {
      LOG << "on_detached";
    } else {
      ERROR << "on_detached: " << status;
    }
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
  if (!OK(status)) {
    ERROR << status;
    return;
  }
}

struct MemoryViewerToy : ObjectToy {
  ui::Button btn_attach;

  MemoryViewerToy(Widget* parent, MemoryViewer& mv) : ObjectToy(parent, mv), btn_attach(this) {
    btn_attach.child = MakeShapeWidget(&btn_attach, PathFromSVG(kPlayShape), "#f56a00"_color);
    btn_attach.UpdateChildTransform();
    btn_attach.target = mv.start.Bind();
    btn_attach.local_to_parent = SkM44(SkMatrix::Translate(-5_cm, -5_cm));
  }

  constexpr static Rect kShapeRect = Rect::MakeCenterZero(10_cm, 10_cm);
  SkPath Shape() const override { return SkPath::Rect(kShapeRect); }

  Vec<sk_sp<SkImage>> images;

  Tock Tick(time::Timer&) override {
    auto mv = LockObject<MemoryViewer>();
    auto& m = mv->mem_meld;

    // Goal: divide memory into consecutive chunks with:
    // - identical memory protection
    // - same backing file
    images.clear();
    if (m.Local().Empty()) return Tock::Draw;  // nothing to animate
    char* begin = &*m.Local().begin();
    char* end = &*m.Local().end();
    auto* current_region = m.Find(m.ToRemote(begin));
    auto current = current_region->local;
    auto CurrentToImage = [&]() {
      int pixels = current.size_bytes() / 4;
      int width = std::max(8, ((int)sqrt(pixels)) & ~7);
      int height = pixels / width;
      if (width > 0 && height > 0) {
        auto info =
            SkImageInfo::Make(SkISize(width, height), kRGB_888x_SkColorType, kOpaque_SkAlphaType);
        // auto info = SkImageInfo::MakeN32(width, height, kUnpremul_SkAlphaType);
        auto pixmap = SkPixmap(info, current.data(), width * 4);
        images.emplace_back(SkImages::RasterFromPixmap(pixmap, nullptr, nullptr));
      }
    };
    // Invariant:
    // - we maintain a 'current' segment
    // - every iteration consumes one region of local memory
    //   A) if it's a continuation - it appends it to the 'current' segment
    //   B) if it's discontinuous - it converts the 'current' into SkImage & advances current
    while (current.data() + current.size() < end) {
      char* next = current.data() + current.size_bytes();
      auto* next_region = m.Find(m.ToRemote(next));

      // TODO: check if the memory segments are continuous in the remote address space
      if (current_region->prot == next_region->prot && current_region->path == next_region->path) {
        // merge 'current' with 'next'
        current = SpanOfArr(current.data(), current.size_bytes() + next_region->local.size_bytes());
      } else {
        // produce an SkImage & replace current
        CurrentToImage();
        current = next_region->local;
        current_region = next_region;
      }
    }
    CurrentToImage();
    return Tock::Drawing;
  }

  void Draw(SkCanvas& canvas) const override {
    auto mv = LockObject<MemoryViewer>();
    SkPaint bg;
    bg.setColor("#e3c2a8"_color);
    canvas.drawPath(shape, bg);

    GetFont().DrawText(canvas, f("size: {}", mv->mem_meld.Local().size_bytes()), {});

    int side = floor(sqrt(images.size()));
    if (side > 0) {
      float cell_width = kShapeRect.Width() / side;
      float cell_height = kShapeRect.Height() / side;
      for (int i = 0; i < images.size(); ++i) {
        auto& img = images[i];
        int column = i % side;
        int row = i / side;
        float left = kShapeRect.left + column * cell_width;
        float right = left + cell_width;
        float bottom = kShapeRect.bottom + row * cell_height;
        float top = bottom + cell_height;
        Rect cell = Rect(left, bottom, right, top);
        canvas.drawImageRect(img.get(), cell, kFastSamplingOptions);
      }
    }

    return BakeChildren(canvas);
  }
};

std::unique_ptr<MemoryViewer::Toy> MemoryViewer::MakeToy(ui::Widget* parent) {
  return make_unique<MemoryViewerToy>(parent, *this);
}

}  // namespace automat::library
