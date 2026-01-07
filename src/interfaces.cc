// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "interfaces.hh"

#include <mutex>
#include <utility>

#include "animation.hh"
#include "embedded.hh"
#include "global_resources.hh"
#include "time.hh"
#include "units.hh"

namespace automat {

void Interface::Unsync() {
  if (sync_block == nullptr) return;
  auto old_block = std::move(sync_block);  // keep mutex alive
  auto lock = std::unique_lock(old_block->mutex);
  auto& members = old_block->members;
  for (int i = 0; i < members.size(); ++i) {
    auto* member = members[i].GetValueUnsafe();
    if (member == this) {
      members.erase(members.begin() + i);
      break;
    }
  }
  OnUnsync();
}

Ptr<SyncBlock> Interface::Sync() {
  if (sync_block == nullptr) {
    sync_block = MAKE_PTR(SyncBlock);
    OnSync();
  }
  return sync_block;
}

// Tells that this interface will receive notifications (receive-only sync)
void SyncBlock::AddSink(NestedPtr<Interface>& member) {
  auto guard = std::unique_lock(mutex);
  for (int i = 0; i < members.size(); ++i) {
    if (members[i] == member) {
      return;
    }
  }
  members.push_back(member);
}

// Tells that this interface is the source of activity / notifications (notify-only sync)
void SyncBlock::AddSource(Interface& interface) {
  if (interface.sync_block == nullptr) {
    interface.sync_block = AcquirePtr();
  } else if (interface.sync_block == this) {
    return;
  } else {
    // It's possible that the old_block is referenced by some unknown Send-Only syncables. We
    // skip them here.
    auto old_block = std::move(interface.sync_block);
    auto guard = std::scoped_lock(old_block->mutex, mutex);
    auto& old_members = old_block->members;
    while (!old_members.empty()) {
      members.emplace_back(std::move(old_members.back()));
      old_members.pop_back();
      if (members.back().GetValueUnsafe()->sync_block == old_block) {
        members.back().GetValueUnsafe()->sync_block = AcquirePtr();
      }
    }
  }
}

void SyncBlock::FullSync(NestedPtr<Interface>& member) {
  AddSink(member);
  AddSource(*member);
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

std::unique_ptr<Object::WidgetInterface> SyncBlock::MakeWidget(ui::Widget* parent) {
  return std::make_unique<SyncBlockWidget>(*this, parent);
}

}  // namespace automat
