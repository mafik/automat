#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <cstdint>
#include <utility>

#include "fn.hpp"
#include "fn_inline.hpp"
#include "fn_ref.hpp"
#include "int.hpp"

namespace automat::mortal_priv {

enum class Kind : U8 { Ptr, Ptr16, Fn, FnRef, Fn40, List };

// Intrusive linked list for Mortal* utilities.
struct Ref {
  Ref* next = nullptr;  // Kind stored in the three least significant bits.
};

// Doubly-linked version of Ref. Allows unlink in O(1).
struct FastRef : Ref {
  Ref** pprev = nullptr;  // points at the 'next' pointer of the previous node (*pprev == this)
};

inline Ref* Untag(Ref* tagged) {
  return reinterpret_cast<Ref*>(reinterpret_cast<uintptr_t>(tagged) & ~uintptr_t{7});
}
inline Kind KindOf(Ref* tagged) {
  return static_cast<Kind>(reinterpret_cast<uintptr_t>(tagged) & 7);
}
template <Kind kind>
Ref* Tag(Ref* node) {
  return reinterpret_cast<Ref*>(reinterpret_cast<uintptr_t>(node) | static_cast<uintptr_t>(kind));
}
constexpr bool HasPprev(Kind kind) { return kind != Kind::Ptr16; }

struct PtrNode : FastRef {
  void* mortal = nullptr;
};
struct Ptr16Node : Ref {
  void* mortal = nullptr;
};
struct FnNode : FastRef {
  Fn<void()> fn;
};
struct FnRefNode : FastRef {
  FnRef<void()> fn;
};
struct Fn40Node : FastRef {
  FnInline<void(), 40> fn;
};
struct ListNode : FastRef {
  void* mortal = nullptr;
  void* colony = nullptr;
  void (*erase)(void* colony, Ref* self) = nullptr;
};

static_assert(alignof(Ref) >= 8);
static_assert(sizeof(PtrNode) == 24);
static_assert(sizeof(Ptr16Node) == 16);

// Insert `node` (of `kind`) at the head of the list whose head slot is `*head_slot`.
template <Kind kind>
void Link(Ref** head_slot, Ref* node) {
  Ref* old_head = *head_slot;
  node->next = old_head;
  if constexpr (HasPprev(kind)) {
    static_cast<FastRef*>(node)->pprev = head_slot;
  }
  if (Ref* first = Untag(old_head); first && HasPprev(KindOf(old_head))) {
    static_cast<FastRef*>(first)->pprev = &node->next;
  }
  *head_slot = Tag<kind>(node);
}

void UnlinkFast(FastRef* node);
void UnlinkSlow(Ref** head_slot, Ref* node);

// Take over `other`'s position in its list, leaving `other` unlinked. The payload is moved
// separately by the caller.
template <Kind kind>
void StealFast(FastRef* self, FastRef* other) {
  self->next = other->next;
  self->pprev = other->pprev;
  if (self->pprev) {
    *self->pprev = Tag<kind>(self);
    if (Ref* succ = Untag(self->next); succ && HasPprev(KindOf(self->next))) {
      static_cast<FastRef*>(succ)->pprev = &self->next;
    }
  }
  other->next = nullptr;
  other->pprev = nullptr;
}

template <typename Node, Kind kind>
struct Callback : Node {
  Callback() = default;
  Callback(const Callback&) = delete;
  Callback& operator=(const Callback&) = delete;
  Callback(Callback&& o) {
    StealFast<kind>(this, &o);
    this->fn = std::move(o.fn);
  }
  Callback& operator=(Callback&& o) {
    if (this != &o) {
      Reset();
      StealFast<kind>(this, &o);
      this->fn = std::move(o.fn);
    }
    return *this;
  }
  ~Callback() { Reset(); }

 protected:
  void LinkInto(Ref** head_slot) { Link<kind>(head_slot, this); }

 private:
  void Reset() {
    if (this->pprev) UnlinkFast(this);
    this->next = nullptr;
    this->pprev = nullptr;
    this->fn = {};
  }
};

}  // namespace automat::mortal_priv
