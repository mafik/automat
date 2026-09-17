#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "audio.hpp"
#include "casting.hpp"
#include "control_flow.hpp"
#include "deserializer.hpp"
#include "error_flames.hpp"
#include "ptr.hpp"
#include "spin_lock.hpp"
#include "string_multimap.hpp"
#include "toy.hpp"
#include "vec.hpp"
#include "widget.hpp"

namespace automat {

struct Connection;
struct Location;
struct Pointer;
struct Container;
struct ObjectSerializer;
struct ObjectDeserializer;
struct ObjectToy;

struct OwnerLink {
  Object* owner;
  OwnerLink* prev = nullptr;
  OwnerLink* next = nullptr;

  void Link(Object& target);
  void Unlink(Object& target);
  void TakeOver(OwnerLink& moved, Object& target);
};

template <typename T>
struct Owned : OwnerLink {
  Ptr<T> ptr;

  explicit Owned(Object& owner) : OwnerLink{&owner} {}
  Owned(Object& owner, Ptr<T> target) : OwnerLink{&owner}, ptr(std::move(target)) {
    if (ptr) Link(*ptr);
  }
  Owned(Owned&& that) noexcept : OwnerLink{that.owner}, ptr(std::move(that.ptr)) {
    if (ptr) TakeOver(that, *ptr);
  }
  Owned(const Owned&) = delete;
  ~Owned() {
    if (ptr) Unlink(*ptr);
  }

  Owned& operator=(Owned&& that) noexcept {
    Reset();
    ptr = std::move(that.ptr);
    if (ptr) TakeOver(that, *ptr);
    return *this;
  }
  Owned& operator=(Ptr<T> target) {
    Reset();
    ptr = std::move(target);
    if (ptr) Link(*ptr);
    return *this;
  }
  Owned& operator=(const Owned&) = delete;

  void Reset() {
    if (ptr) Unlink(*ptr);
    ptr = nullptr;
  }
  [[nodiscard]] Ptr<T> Release() {
    if (ptr) Unlink(*ptr);
    return std::move(ptr);
  }

  T* Get() const { return ptr.Get(); }
  T* operator->() const { return ptr.Get(); }
  T& operator*() const { return *ptr; }
  explicit operator bool() const { return ptr != nullptr; }
  operator const Ptr<T>&() const { return ptr; }
  bool operator==(const T* that) const { return ptr == that; }
};

// Objects are interactive pieces of data & behavior.
//
// Instances of this class provide their logic.
// Appearance is delegated to Widgets.
struct Object : public ReferenceCounted {
  // Enables cast<Object> / dyn_cast<Object>
  static bool classof(const ReferenceCounted* rc) { return dynamic_cast<const Object*>(rc); }

  // Incremented when object state changes. UI-side Toys observe this to know when
  // to wake up and pull fresh state. Readable through WeakPtr without locking
  // because memory survives until weak_refs hits 0.
  AtomicCounter monitor = 0;

  // Used during initialization & to prevent feedback loops in synchronization.
  //
  // While an object is suspended, it should not produce any side-effects.
  bool suspended = false;

  void OnUnsuspend() {}

  void Unsuspend() {
    suspended = false;
    OnUnsuspend();
  }

  SpinLock owners_lock;

  // Note: 2 bytes of padding here
  OwnerLink* owners = nullptr;

  // Bump the counter to notify Toys that state has changed.
  void WakeToys() { monitor.fetch_add(1, std::memory_order_relaxed); }

  Object() = default;

  Object(const Object&) : ReferenceCounted(), monitor(0) {}

  // Create a copy of this object.
  //
  // Subclasses of Object should have a static `proto` field, holding their own
  // prototype instance. This prototype will be cloned when new objects are
  // created. See the `Board::Create` function for details.
  virtual Ptr<Object> Clone() const = 0;

  // Release the memory occupied by this object.
  virtual ~Object();

  // # Interfaces & Serialization
  //
  // Objects serialize themselves to the "value" property within the location objects
  // - Each interface may want to emit multiple properties

  virtual void SerializeState(ObjectSerializer& writer) const;

  // Deserializes a single field of the object. Returns true if the key was handled.
  // Called from persistence.cpp for each key in the object's JSON representation.
  // Derived classes should override this to handle their own fields.
  virtual bool DeserializeKey(ObjectDeserializer& d, StrView key);

  virtual std::string GetText() const { return ""; }
  virtual void SetText(std::string_view text) {}

  virtual Container* AsContainer() { return nullptr; }

  // Visits all interfaces that are members of this object.
  //
  // Lifetime of interfaces is the same as this object. Interfaces should never be deleted as it
  // may create dangling references.
  virtual void Interfaces(const std::function<LoopControl(Interface)>&);

  // Call cb for each interface of type T. cb returns LoopControl to continue or stop early.
  template <typename T>
  void Each(const std::function<LoopControl(T)>& cb) {
    Interfaces([&](Interface iface) {
      if (auto t = dyn_cast<T>(iface)) {
        return cb(t);
      }
      return LoopControl::Continue;
    });
  }

  // Find the first interface of the given type. Returns a null bound type if not found.
  template <typename T>
  T Find() {
    T result;
    Each<T>([&](T t) {
      result = t;
      return LoopControl::Break;
    });
    return result;
  }

  virtual Interface InterfaceFromName(StrView name);

  virtual void Updated(WeakPtr<Object>& updated);

  virtual audio::Sound& NextSound();

  virtual std::partial_ordering operator<=>(const Object& other) const noexcept {
    return GetText() <=> other.GetText();
  }

  using Toy = ObjectToy;

  virtual std::unique_ptr<Toy> MakeToy(ui::Widget* parent);

  // Used to report errors within this object. If an error was caused by some other
  // "error reporter", take a look at ReportError in error.hpp.
  void ReportError(std::string_view message,
                   std::source_location location = std::source_location::current());

  // Clears the error reported by the object itself
  void ClearOwnError();

  SmallVec<Ptr<Object>, 4> Owners();

  Ptr<Object> HomeOwner();

  void MakeHome(Object& owner);

  Ptr<Location> HomeLocation();
};

std::unique_ptr<Action> PickUp(ui::Pointer&, Location&, Object&);

// Provides sensible defaults for most object widgets. Designed to be inherited and tweaked.
//
// It's rendered as a green box with the name of the object.
struct ObjectToy : Toy {
  ObjectToy(Widget* parent, Object& obj) : Toy(parent, obj, nullptr, obj.monitor) {}

  virtual float Width() const;
  virtual std::string Text() const { return std::string(Name()); }
  SkPath Shape() const override;
  void Draw(SkCanvas&) const override;
  void FillMenu(ui::Pointer&, Menu&) override;
  Interface ParentLocation();

  // Reports 1 but if the object is iconified, it checks the CoarseBounds()
  // and returns a scale that would fit in a 1x1cm square.
  //
  // Subclasses that do proper iconification may disable this and always return 1.
  virtual float GetBaseScale() const;

  // Places where the connections to this widget may terminate.
  // Local (metric) coordinates.
  virtual void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const;

  // Returns the start position of the given argument.
  // If coordinate_space is nullptr, returns local (metric) coordinates.
  // If coordinate_space is provided, returns coordinates in that widget's space.
  virtual Vec2AndDir ArgStart(const Interface::Table&);

  Vec2AndDir ArgStart(const Interface::Table&, ui::Widget* coordinate_space);

  // When iconified, prevent children from receiving pointer events.
  bool AllowChildPointerEvents(ui::Widget&) const override;

  std::unique_ptr<ErrorFlames> error_flames;
  void UpdateErrorFlames();

  // Warning: if you ever override this, make sure to call ObjectToy::OnWake()!
  void OnWake() final { UpdateErrorFlames(); }

  template <typename T>
  Ptr<T> LockObject() const {
    return LockOwner<T>();
  }
};

inline std::unique_ptr<ObjectToy> Object::MakeToy(ui::Widget* parent) {
  return std::make_unique<ObjectToy>(parent, *this);
}

static_assert(ToyMaker<Object>);

struct ObjectSerializer : Serializer {
  using Serializer::Serializer;

  std::unordered_set<Str> assigned_names;
  std::unordered_map<Object*, Str> object_to_name;
  std::vector<Object*> serialization_queue;

  Str& ResolveName(Object&, StrView hint = ""sv);
  Str ResolveName(Object&, Interface::Table*, StrView hint = ""sv);
  void Serialize(Object&);
};

struct ObjectDeserializer : Deserializer {
  string_map<Ptr<Object>> objects;

  using Deserializer::Deserializer;

  void RegisterObject(StrView name, Object& object);

  Object* LookupObject(StrView name);
  Locked<Interface> LookupInterface(StrView name);
};

}  // namespace automat
