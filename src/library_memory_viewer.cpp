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
#include "log.hpp"
#include "math.hpp"
#include "mem_meld.hpp"
#include "object.hpp"
#include "root_widget.hpp"
#include "run_button.hpp"
#include "sincos.hpp"
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
  constexpr int pid = 1030745;
  FD pidfd = syscall(SYS_pidfd_open, 1030745, 0);
  if (!pidfd.Opened()) {
    Status status;
    AppendErrorMessage(status) += "Couldn't open pidfd";
    ERROR << status;
    return;
  }

  LOG << "Pidfd=" << pidfd;
  Status status;
  mirror.on_attached = []() { LOG << "on_attached"; };
  mirror.on_changed = [&](const MemMeld::Region& before, const MemMeld::Region& after) {
    LOG << "on_changed";
    if (before.remote == after.remote)
      LOG << "  @" << f("{}", (void*)before.remote);
    else
      LOG << "  @" << f("{}", (void*)before.remote) << " => @" << f("{}", (void*)after.remote);
    // if (before.)
    if (!before.path.empty()) LOG << "  path [" << before.path << "] => [" << after.path << "]";
    last_region = after;
  };
  mirror.on_detached = [](const Status& status) {
    if (OK(status)) {
      LOG << "on_detached";
    } else {
      ERROR << "on_detached: " << status;
    }
  };
  mirror.on_forked = [](int pid) { LOG << "on_forked " << pid; };
  mirror.on_mapped = [&](const MemMeld::Region& region) {
    LOG << "on_mapped " << region;
    last_region = region;
  };
  mirror.Attach(pidfd, status);
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

  Tock Tick(time::Timer&) override { return Tock::Drawing; }

  void Draw(SkCanvas& canvas) const override {
    auto mv = LockObject<MemoryViewer>();
    SkPaint bg;
    bg.setColor("#e3c2a8"_color);
    canvas.drawPath(shape, bg);

    GetFont().DrawText(canvas, f("size: {}", mv->mirror.Mirror().size_bytes()), {});

    auto span = mv->mirror.Mirror();
    span = span.subspan(0, (span.size_bytes() * 1 / 16));
    int pixels = span.size_bytes() / 4;
    int width = std::max(8, ((int)sqrt(pixels)) & ~7);
    int height = pixels / width;
    if (width > 0 && height > 0) {
      auto info =
          SkImageInfo::Make(SkISize(width, height), kRGB_888x_SkColorType, kOpaque_SkAlphaType);
      // auto info = SkImageInfo::MakeN32(width, height, kUnpremul_SkAlphaType);
      auto pixmap = SkPixmap(info, span.data(), width * 4);
      auto img = SkImages::RasterFromPixmap(pixmap, nullptr, nullptr);
      canvas.drawImageRect(img.get(), kShapeRect, kFastSamplingOptions);
    }

    return BakeChildren(canvas);
  }
};

std::unique_ptr<MemoryViewer::Toy> MemoryViewer::MakeToy(ui::Widget* parent) {
  return make_unique<MemoryViewerToy>(parent, *this);
}

}  // namespace automat::library
