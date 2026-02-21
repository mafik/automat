#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <concepts>
#include <functional>
#include <type_traits>

#include "control_flow.hh"
#include "str.hh"

namespace automat {

struct Object;

// Utility: match_const_t<From, To> — propagates const from From to To.
template <typename From, typename To>
using match_const_t =
    std::conditional_t<std::is_const_v<std::remove_reference_t<From>>, const To, To>;

// Interface is the base class for parts of Objects that can be exposed to other Objects.
//
// # Architecture (v2)
//
// Interface::Table holds the static data (Kind, name, state_off, function pointers).
// It is the base class for all interface subtypes (Argument, Syncable, OnOff, etc.).
// With the static inline pattern, each Table is a class-level static — zero per-instance overhead.
//
// Interface itself is a lightweight bound type (Object* + Table*) used for typed access
// to an object's interface. It is constructed on-the-fly when needed.
//
// # Notable subclasses of Table
//
// - Argument (argument.hh/cc) - allows objects to link to (interfaces of) other objects
// - ImageProvider (image_provider.hh) - allows objects to provide image data
//
// # Purpose
//
// 1. Interfaces allow Objects to act in a *generic* way.
// 2. Interfaces allow basic code reuse across Objects.
//
// Objects expose their interfaces using Object::Interfaces function. Automat infrastructure uses
// this to automatically populate menus, help with (de)serialization of state, visualize connections
// between interfaces etc.
//
// Interfaces are identified by their memory addresses. With the static inline pattern, each
// Interface is a class-level static — zero per-instance overhead.
struct Interface {
  enum Kind {
    // Argument and its subclasses (range: kArgument..kLastArgument)
    kArgument,
    kNextArg,      // also an Argument
    kSyncable,     // also an Argument (via Syncable)
    kOnOff,        // also a Syncable
    kLongRunning,  // also an OnOff
    kLastOnOff = kLongRunning,
    kRunnable,  // also a Syncable
    kLastArgument = kRunnable,
    // Standalone interfaces
    kImageProvider,
  };

  struct Table {
    Kind kind;
    int state_off = 0;  // byte offset from Object* to Interface::State
    StrView name;

    constexpr Table(Kind kind, StrView name, int state_off = 0)
        : kind(kind), state_off(state_off), name(name) {}
  };

  struct DefBase {
    template <typename T>
    typename T::Bound Bind(this const T& self) {
      auto* obj = reinterpret_cast<Object*>(reinterpret_cast<char*>(const_cast<T*>(&self)) -
                                            T::Impl::Offset());
      auto& tbl = T::GetTable();
      return typename T::Bound(*obj, tbl);
    }

    // Arrow operator for accessing Bound methods: `def->IsOn()`, `def->TurnOn()`, etc.
    template <typename T>
    auto operator->(this const T& self) {
      struct Proxy {
        typename T::Bound bound;
        typename T::Bound* operator->() { return &bound; }
      };
      return Proxy{self.Bind()};
    }

    template <typename T>
    operator Table&(this const T&) {
      return T::GetTable();
    }

    template <typename T>
    friend bool operator==(const Table* lhs, const T& rhs)
      requires std::derived_from<T, DefBase>
    {
      return lhs == &T::GetTable();
    }

    template <typename T>
    friend bool operator==(const T& lhs, const Table* rhs)
      requires std::derived_from<T, DefBase>
    {
      return &T::GetTable() == rhs;
    }
  };

  Object* object_ptr = nullptr;
  Table* table_ptr = nullptr;

  Interface() = default;
  Interface(Object& obj) : object_ptr(&obj) {}
  Interface(Object& obj, Table& table) : object_ptr(&obj), table_ptr(&table) {}
  Interface(Object* obj, Table* table) : object_ptr(obj), table_ptr(table) {}

  explicit operator bool() const { return object_ptr != nullptr; }

  StrView Name() const { return table_ptr->name; }

  // Typed parent access via deducing this.
  // Subclasses define `using Parent = SomeObject;` and Offset().
  // Then object() returns a reference to the parent Object subclass.
  template <typename T>
  auto& object(this T& iface) {
    return static_cast<match_const_t<T, typename T::Parent>&>(*iface.object_ptr);
  }
};

// Helper to call GetTable() on a Def member reference and yield the Table&.
// Works for both old-style (Interface::Table subclasses) and new-style (Def<ImplT>) members.
namespace detail {

// Old-style: the member IS a Table subclass. Just return it.
template <typename T>
  requires std::derived_from<T, Interface::Table>
Interface::Table& GetTableRef(T& member) {
  return member;
}

// New-style: the member has a static GetTable() returning a Table subclass reference.
template <typename T>
  requires(!std::derived_from<T, Interface::Table> &&
           requires {
             { T::GetTable() } -> std::convertible_to<Interface::Table&>;
           })
Interface::Table& GetTableRef(T&) {
  return T::GetTable();
}

template <typename... Ts>
void VisitInterfaces(const std::function<LoopControl(Interface::Table&)>& cb, Ts&... members) {
  (void)((cb(GetTableRef(members)) == LoopControl::Break) || ...);
}

}  // namespace detail

}  // namespace automat

#if defined(_MSC_VER)
#define NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

// INTERFACE_BOUND(Type, Base) — generates constructors, TableRef, and StateRef for bound types.
// Requires Table and State to be declared before use. Types without meaningful state should
// define `struct State {};` so the macro compiles. StateRef exists but is unused — zero cost.
#define INTERFACE_BOUND(Type, Base)                                                                \
  Type() = default;                                                                                \
  Type(Object& obj, Table& t) : Base(obj, t) {}                                                    \
  Type(Object* obj, Table* t) : Base(obj, t) {}                                                    \
  struct TableRef {                                                                                \
    Table* operator->() const {                                                                    \
      auto* p =                                                                                    \
          reinterpret_cast<const Type*>(reinterpret_cast<intptr_t>(this) - offsetof(Type, table)); \
      return static_cast<Table*>(p->table_ptr);                                                    \
    }                                                                                              \
    Table& operator*() const { return *operator->(); }                                             \
    operator Table*() const { return operator->(); }                                               \
  } table NO_UNIQUE_ADDRESS;                                                                       \
  struct StateRef {                                                                                \
    State* operator->() const {                                                                    \
      auto* p =                                                                                    \
          reinterpret_cast<const Type*>(reinterpret_cast<intptr_t>(this) - offsetof(Type, state)); \
      auto* addr = reinterpret_cast<char*>(p->object_ptr) + p->table_ptr->state_off;                      \
      return reinterpret_cast<State*>(addr);                                                       \
    }                                                                                              \
    State& operator*() const { return *operator->(); }                                             \
  } state NO_UNIQUE_ADDRESS;

// INTERFACES macro: generates the Interfaces() override from a list of member names.
// Each member can be either an old-style static Interface::Table subclass or a new-style Def<>.
#define INTERFACES(...)                                                                          \
  void Interfaces(const std::function<LoopControl(::automat::Interface::Table&)>& cb) override { \
    ::automat::detail::VisitInterfaces(cb, __VA_ARGS__);                                         \
  }
