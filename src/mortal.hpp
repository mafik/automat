#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <concepts>
#include <cstddef>
#include <utility>

#include "colony.hpp"
#include "fn.hpp"
#include "fn_ref.hpp"
#include "mortal_priv.hpp"

// Utilities for working with "Mortal" objects:
// - managed from a single thread (or while holding some mutex)
// - can be destroyed at any time
// - carry a special `MortalCoil mortal_coil` field
namespace automat {

// Destruction of a MortalCoil sends out a notification to everybody who holds a reference to the
// Mortal. This is what allows the various "Mortal*" utilities to function.
//
// Moveable:
// - updates all tracking MortalPtrs & MortalList entries to the new address
// - does NOT update any non-Mortal ptr (for example lambda captures in MortalFn*)
struct MortalCoil {
  mortal_priv::Ref* head = nullptr;

  MortalCoil() = default;
  MortalCoil(const MortalCoil&) = delete;
  MortalCoil& operator=(const MortalCoil&) = delete;
  MortalCoil(MortalCoil&& other) noexcept;
  MortalCoil& operator=(MortalCoil&& other) noexcept;
  ~MortalCoil();

 private:
  void Clear();
  void AdoptFrom(MortalCoil& other);
};

template <typename T>
concept Mortal = requires(T t) {
  { t.mortal_coil } -> std::same_as<MortalCoil&>;
};

// A 24B pointer, set to null when its Mortal dies.
template <typename T>
struct MortalPtr : mortal_priv::PtrNode {
  MortalPtr() = default;
  MortalPtr(std::nullptr_t) {}
  MortalPtr(T* m) { LinkTo(m); }
  MortalPtr(T& m) { LinkTo(&m); }
  MortalPtr(const MortalPtr& o) { LinkTo(o.Get()); }
  MortalPtr(MortalPtr&& o) { Steal(o); }
  MortalPtr& operator=(const MortalPtr& o) {
    if (this != &o) {
      Reset();
      LinkTo(o.Get());
    }
    return *this;
  }
  MortalPtr& operator=(MortalPtr&& o) {
    if (this != &o) {
      Reset();
      Steal(o);
    }
    return *this;
  }
  MortalPtr& operator=(T* m) {
    Reset();
    LinkTo(m);
    return *this;
  }
  MortalPtr& operator=(std::nullptr_t) {
    Reset();
    return *this;
  }
  ~MortalPtr() { Reset(); }

  T* Get() const { return static_cast<T*>(mortal); }
  T* operator->() const { return Get(); }
  T& operator*() const { return *Get(); }
  operator T*() const { return Get(); }
  explicit operator bool() const { return mortal != nullptr; }

 private:
  void LinkTo(T* m) {
    mortal = m;
    if (m) mortal_priv::Link<mortal_priv::Kind::Ptr>(&m->mortal_coil.head, this);
  }
  void Reset() {
    if (pprev) mortal_priv::UnlinkFast(this);
    next = nullptr;
    pprev = nullptr;
    mortal = nullptr;
  }
  void Steal(MortalPtr& o) {
    mortal_priv::StealFast<mortal_priv::Kind::Ptr>(this, &o);
    mortal = o.mortal;
    o.mortal = nullptr;
  }
};

// A smaller (16B) twin of MortalPtr. The price is that it unlinks in O(n).
template <typename T>
struct MortalPtr16 : mortal_priv::Ptr16Node {
  MortalPtr16() = default;
  MortalPtr16(std::nullptr_t) {}
  MortalPtr16(T* m) { LinkTo(m); }
  MortalPtr16(T& m) { LinkTo(&m); }
  MortalPtr16(const MortalPtr16& o) { LinkTo(o.Get()); }
  MortalPtr16(MortalPtr16&& o) {
    LinkTo(o.Get());
    o.Reset();
  }
  MortalPtr16& operator=(const MortalPtr16& o) {
    if (this != &o) {
      Reset();
      LinkTo(o.Get());
    }
    return *this;
  }
  MortalPtr16& operator=(MortalPtr16&& o) {
    if (this != &o) {
      Reset();
      LinkTo(o.Get());
      o.Reset();
    }
    return *this;
  }
  MortalPtr16& operator=(T* m) {
    Reset();
    LinkTo(m);
    return *this;
  }
  MortalPtr16& operator=(std::nullptr_t) {
    Reset();
    return *this;
  }
  ~MortalPtr16() { Reset(); }

  T* Get() const { return static_cast<T*>(mortal); }
  T* operator->() const { return Get(); }
  T& operator*() const { return *Get(); }
  operator T*() const { return Get(); }
  explicit operator bool() const { return mortal != nullptr; }

 private:
  void LinkTo(T* m) {
    mortal = m;
    if (m) mortal_priv::Link<mortal_priv::Kind::Ptr16>(&m->mortal_coil.head, this);
  }
  void Reset() {
    if (mortal) mortal_priv::UnlinkSlow(&Get()->mortal_coil.head, this);
    next = nullptr;
    mortal = nullptr;
  }
};

// Owns an Fn invoked when its Mortal dies.
template <Mortal T>
struct MortalFn : mortal_priv::Callback<mortal_priv::FnNode, mortal_priv::Kind::Fn> {
  MortalFn() = default;
  MortalFn(T& m, Fn<void()> f) {
    this->fn = std::move(f);
    this->LinkInto(&m.mortal_coil.head);
  }
};

// Holds a non-owning FnRef invoked when its Mortal dies. The callable must outlive the Mortal.
template <Mortal T>
struct MortalFnRef : mortal_priv::Callback<mortal_priv::FnRefNode, mortal_priv::Kind::FnRef> {
  MortalFnRef() = default;
  MortalFnRef(T& m, FnRef<void()> f) {
    this->fn = f;
    this->LinkInto(&m.mortal_coil.head);
  }
};

// Owns a callable stored inline (up to 40B, no heap) invoked when its Mortal dies.
template <Mortal T>
struct MortalFn40 : mortal_priv::Callback<mortal_priv::Fn40Node, mortal_priv::Kind::Fn40> {
  MortalFn40() = default;
  template <typename F>
    requires std::invocable<F&>
  MortalFn40(T& m, F&& f) {
    this->fn = std::forward<F>(f);
    this->LinkInto(&m.mortal_coil.head);
  }
};

// A collection of Mortal references. The entries are removed when the Mortals die.
// Backed by Colony, so entry addresses are stable.
template <typename T>
struct MortalList {
 private:
  struct Entry : mortal_priv::ListNode {
    ~Entry() {
      if (pprev) mortal_priv::UnlinkFast(this);
    }
  };
  static void EraseEntry(void* c, mortal_priv::Ref* self) {
    auto* col = static_cast<Colony<Entry>*>(c);
    col->erase(col->get_iterator(static_cast<Entry*>(self)));
  }
  Colony<Entry> colony;

 public:
  MortalList() = default;
  MortalList(const MortalList&) = delete;
  MortalList& operator=(const MortalList&) = delete;

  void Add(T& target) {
    Entry* entry = &*colony.emplace();
    entry->mortal = &target;
    entry->colony = &colony;
    entry->erase = &EraseEntry;
    mortal_priv::Link<mortal_priv::Kind::List>(&target.mortal_coil.head, entry);
  }

  // Remove the entry for `target` while it is still alive (removal on death is automatic). O(n).
  void Erase(T& target) {
    for (auto it = colony.begin(); it != colony.end(); ++it) {
      if (static_cast<T*>(it->mortal) == &target) {
        if (it->pprev) {
          mortal_priv::UnlinkFast(&*it);
          it->pprev = nullptr;
        }
        colony.erase(it);
        return;
      }
    }
  }

  bool empty() const { return colony.empty(); }
  size_t size() const { return colony.size(); }

  void Clear() {
    for (auto& entry : colony) {
      if (entry.pprev) {
        mortal_priv::UnlinkFast(&entry);
        entry.pprev = nullptr;
      }
    }
    colony.clear();
  }

  struct Iterator {
    typename Colony<Entry>::iterator it;
    T& operator*() const { return *static_cast<T*>(it->mortal); }
    T* operator->() const { return static_cast<T*>(it->mortal); }
    Iterator& operator++() {
      ++it;
      return *this;
    }
    bool operator==(const Iterator& o) const { return it == o.it; }
  };
  Iterator begin() { return {colony.begin()}; }
  Iterator end() { return {colony.end()}; }
};

}  // namespace automat
