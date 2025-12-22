// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <shared_mutex>
#include <vector>

#include "format.hh"
#include "ptr.hh"

namespace automat {

// Some objects within Automat may provide interfaces that can be "synced". A synced interface
// allows several objects that follow some interface to act as one.
//
// FusableInterface should be subclassed as a specific abstract interface (SAI) before it's used by
// Objects within Automat.
//
// For each command-like method a specific abstract interface should provide a protected virtual
// entry point, whose name starts with "On". It's intended to be overridden by a concrete
// implementation.
//
// In addition to that, each SAI should also provide two non-virtual ways to call the method:
// - as a command - these methods should follow verb-like names, like "TurnOn", "Increment".
//   A "Do" prefix may be used if a good verb is not available. This method should use the
//   "ForwardDo" helper to forward the call to all syced interface implementations.
// - as a notification - these methods should start with "Notify". This method should use the
//   "ForwardNotify" helper to forward the call to _other_ synced interface implementations.
//
// The distinction between "Do" commands and "Notify" notifications allows interfaces that track
// external state to interoperate with other Automat objects without sending redundant commands to
// their externally tracked objects.
//
// IMPORTANT: To actually make this work, the "On" entry points should not be used directly (only
// through the "ForwardDo" & "ForwardNotify" wrappers). Whenever the "On" entry point us used
// directly, it's not going to be propagated to the other synced implementations.
struct SyncableInterface {
  // TODO: it's ReferenceCounted & mutex-protected already - so maybe turn this into an Object?
  struct SyncBlock : ReferenceCounted {
    std::shared_mutex mutex;
    std::vector<SyncableInterface*> members;
  };

  Ptr<SyncBlock> sync_block = nullptr;

  template <class Self>
  void ForwardDo(this Self& self, auto&& lambda) {
    if (self.sync_block) {
      auto lock = std::shared_lock(self.sync_block->mutex);
      for (auto* other : self.sync_block->members) {
        lambda(*static_cast<Self*>(other));
      }
    } else {
      lambda(self);
    }
  }

  template <class Self>
  void ForwardNotify(this Self& self, auto&& lambda) {
    if (self.sync_block) {
      auto lock = std::shared_lock(self.sync_block->mutex);
      for (auto* other : self.sync_block->members) {
        if (auto other_cast = static_cast<Self*>(other); other_cast != &self) {
          lambda(*other_cast);
        }
      }
    }
  }
};

struct OnOff : SyncableInterface {
  virtual ~OnOff() = default;

  virtual bool IsOn() const = 0;

  void TurnOn() {
    ForwardDo([](OnOff& self) { self.OnTurnOn(); });
  }

  void NotifyTurnedOn() {
    ForwardNotify([](OnOff& self) { self.OnTurnOn(); });
  }

  void TurnOff() {
    ForwardDo([](OnOff& self) { self.OnTurnOff(); });
  }

  void NotifyTurnedOff() {
    ForwardNotify([](OnOff& self) { self.OnTurnOff(); });
  }

  void Toggle() {
    if (IsOn())
      TurnOff();
    else
      TurnOn();
  }

 protected:
  virtual void OnTurnOn() = 0;
  virtual void OnTurnOff() = 0;
};

// TODO: figure out a better name for this
struct InterfaceProvider {
  // The name for objects of this type. English proper noun, UTF-8, capitalized.
  // For example: "Text Editor".
  virtual StrView Name() const {
    const std::type_info& info = typeid(*this);
    return CleanTypeName(info.name());
  }

  virtual OnOff* AsOnOff() { return nullptr; }
};

}  // namespace automat
