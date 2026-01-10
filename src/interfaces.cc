// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "interfaces.hh"

#include <mutex>

#include "animation.hh"
#include "embedded.hh"
#include "global_resources.hh"
#include "status.hh"
#include "time.hh"
#include "units.hh"

namespace automat {

void Interface::Unsync() {
  auto sync_block = end.LockAs<SyncBlock>();
  if (!sync_block) return;
  auto lock = std::unique_lock(sync_block->mutex);

  auto& sources = sync_block->sources;
  for (int i = 0; i < sources.size(); ++i) {
    auto* source = sources[i].GetUnsafe();
    if (source == this) {
      sources.erase(sources.begin() + i);
      break;
    }
  }

  auto& sinks = sync_block->sinks;
  for (int i = 0; i < sinks.size(); ++i) {
    auto* sink = sinks[i].GetUnsafe();
    if (sink == this) {
      sinks.erase(sinks.begin() + i);
      break;
    }
  }

  end.Reset();
  OnUnsync();
}

Ptr<SyncBlock> Sync(NestedPtr<Interface>& source) {
  auto sync_block = source->end.OwnerLockAs<SyncBlock>();
  if (!sync_block) {
    sync_block = MAKE_PTR(SyncBlock);
    sync_block->AddSource(source);
  }
  return sync_block;
}

SyncBlock::~SyncBlock() {
  auto lock = std::unique_lock(mutex);
  while (!sources.empty()) {
    if (auto source = sources.back().Lock()) {
      source->OnUnsync();
    }
    sources.pop_back();
  }
}

// Tells that this interface will receive notifications (receive-only sync)
void SyncBlock::AddSink(NestedPtr<Interface>& sink) {
  auto guard = std::unique_lock(mutex);
  for (int i = 0; i < sinks.size(); ++i) {
    if (sinks[i] == sink) {
      return;
    }
  }
  sinks.push_back(sink);
}

// Tells that this interface is the source of activity / notifications (notify-only sync)
void SyncBlock::AddSource(NestedPtr<Interface>& source) {
  auto old_sync_block = source->end.LockAs<SyncBlock>();
  if (old_sync_block.Get() == this) {
    return;
  }
  source->Connect(*source.Owner<Object>(), AcquirePtr());
  sources.push_back(source);
  if (!old_sync_block) {
    source->OnSync();
  }
}

void SyncBlock::FullSync(NestedPtr<Interface>& interface) {
  AddSink(interface);
  AddSource(interface);
}

struct SyncBlockWidget : Object::WidgetBase {
  SyncBlockWidget(SyncBlock& sync_block, Widget* parent) : WidgetBase(parent) {
    object = sync_block.AcquireWeakPtr();
  }

  SkPath Shape() const override { return SkPath::Circle(0, 0, 1_cm); }

  bool CenteredAtZero() const override { return true; }

  animation::Phase Tick(time::Timer& t) override { return animation::Animating; }

  void Draw(SkCanvas& canvas) const override {
    Status status;
    static auto effect = resources::CompileShader(embedded::assets_gear_sksl, status);
    if (!OK(status)) {
      FATAL << status;
    }
    SkRuntimeEffectBuilder builder(effect);
    builder.uniform("iTime") = (float)time::SteadySaw<M_PI * 2>();
    SkMatrix px_to_local;
    (void)canvas.getLocalToDeviceAs3x3().invert(&px_to_local);
    builder.uniform("iPixelRadius") = (float)px_to_local.mapRadius(1);
    SkPaint paint;
    paint.setShader(builder.makeShader());
    canvas.drawCircle(0, 0, 1_cm, paint);
  }
};

std::unique_ptr<ObjectWidget> SyncBlock::MakeWidget(ui::Widget* parent) {
  return std::make_unique<SyncBlockWidget>(*this, parent);
}

void Interface::CanConnect(Object& start, Part& end, Status& status) const {
  if (dynamic_cast<SyncBlock*>(&end) != nullptr) {
    AppendErrorMessage(status) += "Can only connect to SyncBlock";
  }
}

}  // namespace automat
