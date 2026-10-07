#pragma once
// SPDX-FileCopyrightText: Copyright 2026 Automat Authors
// SPDX-License-Identifier: MIT

#include <atomic>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "casting.hpp"
#include "control_flow.hpp"
#include "ptr.hpp"
#include "str.hpp"
#include "vec.hpp"

namespace automat {

struct Action;
struct Object;
struct Toy;
struct Menu;

namespace ui {
struct Pointer;
struct Widget;
enum class Cursor;
}  // namespace ui

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
// Long-term storage of interface pointers relies on Linked<T> & Locked<T>.
// This allows concurrent lifetime management.
//
// # Notable interfaces
//
// - Argument (argument.hpp/cpp) - allows objects to link to (interfaces of) other objects
// - ImageProvider (image_provider.hpp) - allows objects to provide image data
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
    kNextArg,    // sub-kind of InterfaceArgument
    kStreamArg,  // sub-kind of InterfaceArgument; connects to a StreamInput
    kLastInterfaceArgument = kStreamArg,
    kSyncArgument,
    kLastArgument = kSyncArgument,
    // Standalone interfaces
    kCommand,
    kOnOff,
    kLastCommand = kOnOff,
    kSyncable,
    kImageProvider,
    kResizable,
    kStreamInput,
    kFdProvider,
    kObjectSource,
    kScalar,
    kText,
    kMenu,
  };

  struct Table {
    Kind kind;
    int state_off = 0;  // byte offset from Object* to Interface::State
    StrView name;
    ui::Cursor cursor = {};
    std::unique_ptr<Action> (*activate)(Span<Interface>, ui::Pointer&, Toy*) = nullptr;
    void (*fill_menu)(Interface, Table& true_self, Menu&) = nullptr;
    std::unique_ptr<ui::Widget> (*make_icon)(Interface, ui::Widget* parent) = &DefaultMakeIcon;

    constexpr Table(Kind kind, StrView name, int state_off = 0)
        : kind(kind), state_off(state_off), name(name) {}

    static bool classof(const Table*) { return true; }
    static std::unique_ptr<ui::Widget> DefaultMakeIcon(Interface, ui::Widget* parent);

    // FillFrom<ImplT>() — canonical compile-time ImplT-driven initialization.
    // Subtype Tables chain to this via their own FillFrom<ImplT>().
    // For dynamic (runtime) table initialization, assign function pointers directly instead.
    template <typename ImplT>
    constexpr void FillFrom() {
      state_off = ImplT::Offset();
      if constexpr (requires { ImplT::kCursor; }) cursor = ImplT::kCursor;
      if constexpr (requires { &ImplT::OnActivate; })
        static_assert(
            requires(ImplT& i, ui::Pointer& p, Toy* t) { i.OnActivate(p, t); },
            "OnActivate must take (ui::Pointer&, automat::Toy*)");
      if constexpr (requires(ImplT& i, ui::Pointer& p, Toy* t) { i.OnActivate(p, t); })
        activate = [](Span<Interface> chain, ui::Pointer& p, Toy* t) -> std::unique_ptr<Action> {
          return static_cast<ImplT&>(chain.back()).OnActivate(p, t);
        };
      if constexpr (requires(ImplT& i, ui::Widget* p) { i.OnMakeIcon(p); })
        make_icon = [](Interface self, ui::Widget* p) -> std::unique_ptr<ui::Widget> {
          return static_cast<ImplT&>(self).OnMakeIcon(p);
        };
      if constexpr (requires(ImplT& i, Menu& m) { i.OnFillMenu(m); })
        fill_menu = [](Interface self, Table&, Menu& m) {
          static_cast<ImplT&>(self).OnFillMenu(m);
        };
    }
  };

  struct DefBase {
    template <typename T>
    typename T::Bound Bind(this const T& self) {
      constexpr int slot = T::Impl::Parent::SlotOf(T::tbl);
      static_assert(slot != 0);
      auto* obj = reinterpret_cast<decltype(T::Bound::object_ptr)>(
          reinterpret_cast<char*>(const_cast<T*>(&self)) - T::Impl::Offset());
      return obj->template InterfaceFromIdx<typename T::Bound>(slot);
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
  Interface(std::nullptr_t) {}
  Interface(Object& obj) : object_ptr(&obj) {}
  Interface(Object& obj, Table& table) : object_ptr(&obj), table_ptr(&table) {}
  Interface(Object* obj, Table* table) : object_ptr(obj), table_ptr(table) {}

  auto operator<=>(const Interface&) const = default;

  bool has_object() const { return object_ptr != nullptr; }
  bool has_table() const { return table_ptr != nullptr; }
  explicit operator bool() const { return has_object() && has_table(); }

  StrView Name() const { return table_ptr->name; }

  std::unique_ptr<Action> Activate(ui::Pointer&, Toy* toy = nullptr) const;
  void FillMenu(Menu&) const;
  std::unique_ptr<ui::Widget> MakeIcon(ui::Widget* parent) const;

  // TODO: make this a Table's function pointer
  Ptr<Object> MakeController() const;

  std::unique_ptr<Action> DragNewController(ui::Pointer&, ui::Widget& birthplace) const;
};

template <typename T>
static inline void SafeIncrementTocRefs(T* obj) {
  if (obj) obj->toc_refs.fetch_add(1, std::memory_order_acquire);
}

template <typename T>
static inline void SafeDecrementTocRefs(T* obj) {
  if (obj) obj->DecrementTocRefs();
}

// Ref-counted wrapper for bound interface types. Keeps the owner Object alive.
// Inherits from T so it IS-A T (e.g. Locked<Argument> is an Argument).
template <typename T = Interface>
struct Locked : T {
  Locked() = default;
  ~Locked() {
    SafeDecrementTocRefs(this->object_ptr);
    SafeDecrementOwningRefs(this->object_ptr);
  }
  Locked(const Locked& o) : T(o) {
    SafeIncrementOwningRefs(this->object_ptr);
    SafeIncrementTocRefs(this->object_ptr);
  }
  Locked(Locked&& o) : T(o) {
    o.object_ptr = nullptr;
    o.table_ptr = nullptr;
  }
  Locked& operator=(const Locked& o) {
    SafeIncrementOwningRefs(o.object_ptr);
    SafeIncrementTocRefs(o.object_ptr);
    SafeDecrementTocRefs(this->object_ptr);
    SafeDecrementOwningRefs(this->object_ptr);
    this->object_ptr = o.object_ptr;
    this->table_ptr = o.table_ptr;
    return *this;
  }
  Locked& operator=(Locked&& o) {
    if (this != &o) {
      SafeDecrementTocRefs(this->object_ptr);
      SafeDecrementOwningRefs(this->object_ptr);
      this->object_ptr = o.object_ptr;
      this->table_ptr = o.table_ptr;
      o.object_ptr = nullptr;
      o.table_ptr = nullptr;
    }
    return *this;
  }

 private:
  struct AdoptTag {};
  Locked(T bound, AdoptTag) : T(bound) {}

  template <typename U>
  friend Locked<U> AdoptLocked(U bound);

  template <typename To, typename From>
  friend Locked<To> cast(Locked<From>&& from);
};

// Construct a Locked<T> that adopts an existing owning ref (caller has already incremented).
template <typename T>
Locked<T> AdoptLocked(T bound) {
  SafeIncrementTocRefs(bound.object_ptr);
  return Locked<T>(bound, {});
}

template <typename T>
Locked<T> AcquireLocked(T bound) {
  bound.object_ptr->IncrementOwningRefs();
  return AdoptLocked(bound);
}

template <typename To, typename From>
[[nodiscard]] Locked<To> cast(Locked<From>&& from) {
  assert(from.table_ptr == nullptr || isa<To>(static_cast<From&>(from)));
  To bound(from.object_ptr, static_cast<typename To::Table*>(from.table_ptr));
  from.object_ptr = nullptr;
  from.table_ptr = nullptr;
  return Locked<To>(bound, {});
}

template <typename To, typename From>
[[nodiscard]] Locked<To> dyn_cast(Locked<From>&& from) {
  if (!isa<To>(static_cast<From&>(from))) return {};
  return cast<To>(std::move(from));
}

std::unique_ptr<Action> Activate(Vec<Locked<>> chain, ui::Pointer&, Toy*);

template <typename T = Interface>
struct Linked {
  decltype(T::object_ptr) object_ptr = nullptr;
  int toc_idx = 0;

  Linked() = default;
  ~Linked() { SafeDecrementWeakRefs(object_ptr); }
  Linked(std::nullptr_t) {}
  Linked(T bound) : object_ptr(bound.object_ptr) {
    if (object_ptr == nullptr) return;
    toc_idx = object_ptr->InterfaceIdx(bound.table_ptr);
    SafeIncrementWeakRefs(object_ptr);
  }

  template <typename U>
  Linked(const WeakPtr<U>& owner, typename T::Table* table) : object_ptr(owner.GetUnsafe()) {
    if (object_ptr == nullptr) return;
    toc_idx = object_ptr->InterfaceIdx(table);
    SafeIncrementWeakRefs(object_ptr);
  }

  Linked(const Linked& o) : object_ptr(o.object_ptr), toc_idx(o.toc_idx) {
    SafeIncrementWeakRefs(object_ptr);
  }
  Linked(Linked&& o) : object_ptr(o.object_ptr), toc_idx(o.toc_idx) {
    o.object_ptr = nullptr;
    o.toc_idx = 0;
  }
  Linked& operator=(const Linked& o) {
    SafeIncrementWeakRefs(o.object_ptr);
    SafeDecrementWeakRefs(object_ptr);
    object_ptr = o.object_ptr;
    toc_idx = o.toc_idx;
    return *this;
  }
  Linked& operator=(Linked&& o) {
    if (this != &o) {
      SafeDecrementWeakRefs(object_ptr);
      object_ptr = o.object_ptr;
      toc_idx = o.toc_idx;
      o.object_ptr = nullptr;
      o.toc_idx = 0;
    }
    return *this;
  }

  Locked<T> Lock() const {
    if (object_ptr == nullptr) return {};
    if (!object_ptr->IncrementOwningRefsNonZero()) return {};
    {
      typename std::remove_pointer_t<decltype(T::object_ptr)>::TocGuard guard(*object_ptr);
      Interface iface = object_ptr->InterfaceFromIdx(toc_idx);
      if (iface.has_object()) {
        return AdoptLocked(T(iface.object_ptr, static_cast<typename T::Table*>(iface.table_ptr)));
      }
    }
    SafeDecrementOwningRefs(object_ptr);
    return {};
  }

  Interface Unsafe() const {
    if (IsExpired()) return {};
    return object_ptr->InterfaceFromIdx(toc_idx);
  }

  bool IsExpired() const {
    return object_ptr == nullptr || object_ptr->owning_refs.load(std::memory_order_relaxed) == 0;
  }

  void Reset() {
    SafeDecrementWeakRefs(object_ptr);
    object_ptr = nullptr;
    toc_idx = 0;
  }

  explicit operator bool() const { return object_ptr && toc_idx; }
  bool operator==(const Linked&) const = default;
};

constexpr Interface::Table MenuTable(StrView name,
                                     void (*fill_menu)(Interface, Interface::Table&, Menu&)) {
  Interface::Table t(Interface::kMenu, name);
  t.fill_menu = fill_menu;
  return t;
}

Str ToStr(Interface iface);

// Manages list of interfaces supported by an object.
//
// WARNING: Indexing is 1-based! Index 0 is reserved for "whole-object" linking.
struct ToC {
  virtual ~ToC() = default;
  std::span<Interface::Table*> array;

  int size() { return array.size(); }
  Interface::Table** begin() { return &array[0]; }
  Interface::Table** end() { return &array[array.size()]; }
  Interface::Table*& operator[](int i) { return array[i - 1]; }

  int SlotOf(const Interface::Table* table) {
    for (int i = 1; i <= size(); ++i) {
      if ((*this)[i] == table) return i;
    }
    return 0;
  }
};

template <auto&... tables>
struct StaticToC : ToC {
  Interface::Table* buffer[sizeof...(tables)] = {&tables...};
  constexpr StaticToC() { array = std::span<Interface::Table*>(buffer, sizeof...(tables)); }
};

struct HeapToC : ToC {
  std::vector<Interface::Table*> buffer;
  explicit HeapToC(int size) : buffer(size) { array = buffer; }
  HeapToC(const HeapToC& other) : buffer(other.buffer) { array = buffer; }
};

static_assert(sizeof(ToC) == 24);

template <auto&... tables>
inline StaticToC<tables...> toc_of = {};

template <auto&... tables>
constexpr int SlotOf(const Interface::Table& table) {
  const Interface::Table* slots[] = {nullptr, &tables...};
  for (int i = 1; i <= sizeof...(tables); ++i) {
    if (slots[i] == &table) return i;
  }
  return 0;
}

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
  Type(std::nullptr_t) : Base(nullptr) {}                                                          \
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
      char* addr = reinterpret_cast<char*>(p->object_ptr) + p->table_ptr->state_off;               \
      return reinterpret_cast<State*>(addr);                                                       \
    }                                                                                              \
    State& operator*() const { return *operator->(); }                                             \
  } state NO_UNIQUE_ADDRESS;

#define INTERFACES(...)                                                   \
  static constexpr int SlotOf(const ::automat::Interface::Table& table) { \
    return ::automat::SlotOf<__VA_ARGS__>(table);                         \
  }                                                                       \
  static constexpr ToC& base_toc = ::automat::toc_of<__VA_ARGS__>;        \
  ToC* BaseToc() const override { return &base_toc; }                     \
  NO_UNIQUE_ADDRESS ::automat::SetToc toc_initializer { this, base_toc }

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
    using Parent = ParentType;                                                     \
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
