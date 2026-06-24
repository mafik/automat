// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include "mortal.hpp"

#include <cstddef>

namespace automat::mortal_priv {

// Remove a node that stores its own back-link.
void UnlinkFast(FastRef* node) {
  Ref** slot = node->pprev;
  *slot = node->next;
  if (Ref* succ = Untag(node->next); succ && HasPprev(KindOf(node->next))) {
    static_cast<FastRef*>(succ)->pprev = slot;
  }
}

// Remove a node that does not store a back-link, by walking from the head.
void UnlinkSlow(Ref** head_slot, Ref* node) {
  Ref** slot = head_slot;
  while (Untag(*slot) != node) {
    slot = &Untag(*slot)->next;
  }
  *slot = node->next;
  if (Ref* succ = Untag(node->next); succ && HasPprev(KindOf(node->next))) {
    static_cast<FastRef*>(succ)->pprev = slot;
  }
}

}  // namespace automat::mortal_priv

namespace automat {

using namespace mortal_priv;

void MortalCoil::Clear() {
  // Pop from the head before acting, so a callback that destroys another reference into this same
  // list (which then unlinks itself) cannot invalidate the node we are about to visit.
  while (head) {
    Ref* node = Untag(head);
    Kind kind = KindOf(head);
    head = node->next;
    if (Ref* first = Untag(head); first && HasPprev(KindOf(head))) {
      static_cast<FastRef*>(first)->pprev = &head;
    }
    switch (kind) {
      case Kind::Ptr:
        static_cast<FastRef*>(node)->pprev = nullptr;
        static_cast<PtrNode*>(node)->mortal = nullptr;
        break;
      case Kind::Ptr16:
        static_cast<Ptr16Node*>(node)->mortal = nullptr;
        break;
      case Kind::Fn: {
        static_cast<FastRef*>(node)->pprev = nullptr;
        auto& fn = static_cast<FnNode*>(node)->fn;
        if (fn) fn();
        break;
      }
      case Kind::FnRef: {
        static_cast<FastRef*>(node)->pprev = nullptr;
        auto& fn = static_cast<FnRefNode*>(node)->fn;
        if (fn) fn();
        break;
      }
      case Kind::Fn40: {
        static_cast<FastRef*>(node)->pprev = nullptr;
        auto& fn = static_cast<Fn40Node*>(node)->fn;
        if (fn) fn();
        break;
      }
      case Kind::List: {
        auto* entry = static_cast<ListNode*>(node);
        entry->pprev = nullptr;
        entry->erase(entry->colony, entry);
        break;
      }
    }
  }
}

// Take over `other`'s referers without nothing anything: the MortalCoil is relocating, not dying.
//
// The coil sits at a fixed offset within its Mortal so every tracking pointer is shifted by the
// same relocation delta.
void MortalCoil::AdoptFrom(MortalCoil& other) {
  head = other.head;
  other.head = nullptr;
  if (!head) return;
  if (HasPprev(KindOf(head))) {
    static_cast<FastRef*>(Untag(head))->pprev = &head;
  }
  ptrdiff_t delta = reinterpret_cast<char*>(this) - reinterpret_cast<char*>(&other);
  for (Ref* tagged = head; tagged; tagged = Untag(tagged)->next) {
    Ref* node = Untag(tagged);
    switch (KindOf(tagged)) {
      case Kind::Ptr:
        static_cast<PtrNode*>(node)->mortal =
            reinterpret_cast<char*>(static_cast<PtrNode*>(node)->mortal) + delta;
        break;
      case Kind::Ptr16:
        static_cast<Ptr16Node*>(node)->mortal =
            reinterpret_cast<char*>(static_cast<Ptr16Node*>(node)->mortal) + delta;
        break;
      case Kind::List:
        static_cast<ListNode*>(node)->mortal =
            reinterpret_cast<char*>(static_cast<ListNode*>(node)->mortal) + delta;
        break;
      default:
        break;
    }
  }
}

MortalCoil::~MortalCoil() { Clear(); }

MortalCoil::MortalCoil(MortalCoil&& other) noexcept { AdoptFrom(other); }

MortalCoil& MortalCoil::operator=(MortalCoil&& other) noexcept {
  if (this != &other) {
    Clear();           // the overwritten Mortal's old value dies properly (its callbacks fire) ...
    AdoptFrom(other);  // ... before this takes over the source's referers
  }
  return *this;
}

}  // namespace automat
