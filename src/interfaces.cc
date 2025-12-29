// SPDX-FileCopyrightText: Copyright 2025 Automat Authors
// SPDX-License-Identifier: MIT

#include "interfaces.hh"

namespace automat {

void Sync(Object& self_object, Interface& self, Object& other_object, Interface& other) {
  if (self.sync_block == nullptr && other.sync_block == nullptr) {
    auto block = MAKE_PTR(SyncBlock);
    auto lock = std::unique_lock(block->mutex);
    block->members.emplace_back(self_object.AcquireWeakPtr(), &self);
    block->members.emplace_back(other_object.AcquireWeakPtr(), &other);
    self.sync_block = block;
    other.sync_block = std::move(block);
  } else if (self.sync_block != nullptr && other.sync_block != nullptr) {
    if (self.sync_block == other.sync_block) {
      return;
    }
    auto lock = std::scoped_lock(self.sync_block->mutex, other.sync_block->mutex);

    // Move the members from the 'other' block to 'self' block.
    auto& self_block = self.sync_block;
    auto& other_block = other.sync_block;
    auto& self_members = self.sync_block->members;
    auto& other_members = other.sync_block->members;
    while (!other_members.empty()) {
      self_members.emplace_back(std::move(other_members.back()));
      other_members.pop_back();
      self_members.back().GetValueUnsafe()->sync_block = self_block;
    }
  } else {
    if (self.sync_block) {
      auto lock = std::unique_lock(self.sync_block->mutex);
      self.sync_block->members.emplace_back(other_object.AcquireWeakPtr(), &other);
      other.sync_block = self.sync_block;
    } else {
      auto lock = std::unique_lock(other.sync_block->mutex);
      other.sync_block->members.emplace_back(self_object.AcquireWeakPtr(), &self);
      self.sync_block = other.sync_block;
    }
  }
}

}  // namespace automat
