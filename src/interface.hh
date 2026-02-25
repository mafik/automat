#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <concepts>
#include <functional>
#include <type_traits>

#include "casting.hh"
#include "control_flow.hh"
#include "ptr.hh"
#include "str.hh"

namespace automat {

struct Object;

// Interface is the base class for parts of Objects that can be exposed to other Objects.
//
// Interface::Table holds the static data (type, name, state offset, function pointers).
// It is the base class for all interface subtypes (Argument, Syncable, OnOff, etc.).
// With the static inline pattern, each Table is a class-level static — zero per-instance overhead.
//
// Interfaces are identified by the address of their Table but they also provide a tree-shaped
// hierarchy of types through the `kind` field.
//
// Interface itself is a lightweight bound type (Object* + Table*) used for typed access
// to an object's interface. It is constructed on-the-fly when needed.
//
// Long-term storage of interface pointers relies on NestedPtr<Interface::Table> &
// NestedWeakPtr<Interface::Table>. This allows concurrent lifetime management.
//
// # Notable interfaces
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
struct Interface {
  enum Kind {
    // Argument and its subclasses (range: kArgument..kLastArgument)
    kArgument,
    kObjectArgument,
    kInterfaceArgument,
    kNextArg,  // sub-kind of InterfaceArgument
    kLastInterfaceArgument = kNextArg,
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

    static bool classof(const Table*) { return true; }

    // FillFrom<ImplT>() — canonical compile-time ImplT-driven initialization.
    // Subtype Tables chain to this via their own FillFrom<ImplT>().
    // For dynamic (runtime) table initialization, assign function pointers directly instead.
    template <typename ImplT>
    constexpr void FillFrom() {
      state_off = ImplT::Offset();
    }
  };

  struct DefBase {
    template <typename T>
    typename T::Bound Bind(this const T& self) {
      auto* obj = reinterpret_cast<Object*>(reinterpret_cast<char*>(const_cast<T*>(&self)) -
                                            T::Impl::Offset());
      return typename T::Bound(*obj, T::tbl);
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
      return T::tbl;
    }

    template <typename T>
    operator typename T::Bound(this const T& self) {
      return self.Bind();
    }

    template <typename T>
    friend bool operator==(const Table* lhs, const T& rhs)
      requires std::derived_from<T, DefBase>
    {
      return lhs == &T::tbl;
    }

    template <typename T>
    friend bool operator==(const T& lhs, const Table* rhs)
      requires std::derived_from<T, DefBase>
    {
      return &T::tbl == rhs;
    }
  };

  Object* object_ptr = nullptr;
  Table* table_ptr = nullptr;

  Interface() = default;
  Interface(Object& obj) : object_ptr(&obj) {}
  Interface(Object& obj, Table& table) : object_ptr(&obj), table_ptr(&table) {}
  Interface(Object* obj, Table* table) : object_ptr(obj), table_ptr(table) {}

  operator NestedPtr<Table>();
  operator NestedWeakPtr<Table>();

  bool has_object() const { return object_ptr != nullptr; }
  bool has_table() const { return table_ptr != nullptr; }
  explicit operator bool() const { return has_object() && has_table(); }

  StrView Name() const { return table_ptr->name; }
};

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
  operator NestedPtr<Table>() {                                                                    \
    return Interface::operator NestedPtr<Interface::Table>().template Cast<Table>();               \
  }                                                                                                \
  operator NestedWeakPtr<Table>() {                                                                \
    return Interface::operator NestedWeakPtr<Interface::Table>().template Cast<Table>();           \
  }                                                                                                \
  struct StateRef {                                                                                \
    State* operator->() const {                                                                    \
      auto* p =                                                                                    \
          reinterpret_cast<const Type*>(reinterpret_cast<intptr_t>(this) - offsetof(Type, state)); \
      auto* addr = reinterpret_cast<char*>(p->object_ptr) + p->table_ptr->state_off;               \
      return reinterpret_cast<State*>(addr);                                                       \
    }                                                                                              \
    State& operator*() const { return *operator->(); }                                             \
  } state NO_UNIQUE_ADDRESS;

namespace automat::detail {
template <typename... Ts>
void VisitInterfaces(const std::function<LoopControl(Interface)>& cb, Ts&... members) {
  (void)((cb(members.Bind()) == LoopControl::Break) || ...);
}
}  // namespace automat::detail

// INTERFACES macro: generates the Interfaces() override from a list of member names.
#define INTERFACES(...)                                                                  \
  void Interfaces(const std::function<LoopControl(::automat::Interface)>& cb) override { \
    ::automat::detail::VisitInterfaces(cb, __VA_ARGS__);                                 \
  }

// Helper for implementing Interfaces within objects. See `Interfaces.md`.
//
// DEF_INTERFACE(ParentType, InterfaceType, member_name, "Display Name")
//   ReturnType MethodName(...) { ... }
// DEF_END(member_name);
//
// Expands to:
//   struct member_name##_Impl : InterfaceType {
//     using IFaceT = InterfaceType;
//     static constexpr StrView kName = "Display Name";
//     static constexpr int Offset() { return offsetof(ParentType, member_name); }
//     struct ObjRef { ParentType& operator*() const; ParentType* operator->() const; }
//     NO_UNIQUE_ADDRESS ObjRef obj;
//     ReturnType MethodName(...) { ... }
//   };
//   NO_UNIQUE_ADDRESS InterfaceType::Def<member_name##_Impl> member_name
#define DEF_INTERFACE(ParentType, InterfaceType, member_name, display_name)        \
  struct member_name##_Impl : InterfaceType {                                      \
    using IFaceT = InterfaceType;                                                  \
    static constexpr StrView kName = display_name;                                 \
    static constexpr int Offset() { return offsetof(ParentType, member_name); }    \
    struct ObjRef {                                                                \
      ParentType& operator*() const {                                              \
        auto* impl = reinterpret_cast<const member_name##_Impl*>(                  \
            reinterpret_cast<intptr_t>(this) - offsetof(member_name##_Impl, obj)); \
        return static_cast<ParentType&>(*impl->object_ptr);                        \
      }                                                                            \
      ParentType* operator->() const { return &**this; }                           \
    } obj NO_UNIQUE_ADDRESS;

#define DEF_END(member_name)                                                         \
  }                                                                                  \
  ;                                                                                  \
  NO_UNIQUE_ADDRESS member_name##_Impl::IFaceT::Def<member_name##_Impl> member_name; \
  static constexpr auto& member_name##_tbl = decltype(member_name)::tbl

// dyn_cast / isa support for Interface-derived bound types.
//
// Enables: dyn_cast<Syncable>(arg), isa<OnOff>(runnable), etc.
// Returns a nullable bound type (null on failure, checked via operator bool).
//
// The enable_if condition uses a requires-expression, which gracefully fails
// (returns false) for incomplete types — unlike std::is_base_of_v which would
// be a hard error. The condition checks that From has table_ptr and that
// To::Table::classof accepts it, which is true exactly for automat bound types.
namespace llvm {
template <typename To, typename From>
    struct CastInfo < To,
    From, std::enable_if_t < requires(const std::remove_cvref_t<From>& f) {
  To::Table::classof(f.table_ptr);
} >>{static bool isPossible(const From& f){return f.table_ptr != nullptr &&
                                                  To::Table::classof(f.table_ptr);
}  // namespace llvm
static To doCast(From f) { return To(f.object_ptr, static_cast<typename To::Table*>(f.table_ptr)); }
static To castFailed() { return To{}; }
static To doCastIfPossible(From f) {
  if (!isPossible(f)) return castFailed();
  return doCast(f);
}
}
;
}  // namespace llvm
