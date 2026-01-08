// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

#include "audio.hh"
#include "deserializer.hh"
#include "ptr.hh"
#include "widget.hh"

namespace automat {

struct Connection;
struct Location;
struct Pointer;
struct Container;
struct Argument;
struct ImageProvider;
struct OnOff;
struct LongRunning;
struct Interface;

// Objects are interactive pieces of data & behavior.
//
// Instances of this class provide their logic.
// Appearance is delegated to Widgets.
struct Object : public ReferenceCounted {
  Object() {}

  // Create a copy of this object.
  //
  // Subclasses of Object should have a static `proto` field, holding their own
  // prototype instance. This prototype will be cloned when new objects are
  // created. See the `Machine::Create` function for details.
  virtual Ptr<Object> Clone() const = 0;

  virtual void Relocate(Location* new_here) {}

  // Release the memory occupied by this object.
  virtual ~Object();

  virtual void SerializeState(Serializer& writer, const char* key = "value") const;

  // Restores state when Automat is restarted.
  virtual void DeserializeState(Location& l, Deserializer& d);

  virtual std::string GetText() const { return ""; }
  virtual void SetText(Location& error_context, std::string_view text) {}

  // Pointer-like objects can be followed.
  virtual Pointer* AsPointer() { return nullptr; }

  virtual Container* AsContainer() { return nullptr; }

  // Image-like objects can provide image data.
  virtual ImageProvider* AsImageProvider() { return nullptr; }

  virtual LongRunning* AsLongRunning() { return nullptr; }

  virtual operator OnOff*() { return nullptr; }

  virtual Span<Interface*> Interfaces() { return {}; }

  virtual void Args(std::function<void(Argument&)> cb);

  // TODO: move this to Argument
  virtual Ptr<Object> ArgPrototype(const Argument&) { return nullptr; }

  virtual void Updated(Location& here, Location& updated);

  virtual audio::Sound& NextSound();

  virtual std::partial_ordering operator<=>(const Object& other) const noexcept {
    return GetText() <=> other.GetText();
  }

  struct WidgetInterface : ui::Widget {
    using ui::Widget::Widget;

    // Get the default scale that this object would like to have.
    // Usually it's 1 but when it's iconified, it may want to shrink itself.
    virtual float GetBaseScale() const = 0;

    // Places where the connections to this widget may terminate.
    // Local (metric) coordinates.
    virtual void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const = 0;

    // Returns the start position of the given argument.
    // Local (metric) coordinates.
    virtual Vec2AndDir ArgStart(const Argument&) = 0;
  };

  // Provides sensible defaults for most object widgets. Designed to be inherited and tweaked.
  //
  // It's rendered as a green box with the name of the object.
  struct WidgetBase : WidgetInterface {
    WeakPtr<Object> object;

    WidgetBase(Widget* parent) : WidgetInterface(parent) {}

    std::string_view Name() const override;
    virtual float Width() const;
    virtual std::string Text() const { return std::string(Name()); }
    SkPath Shape() const override;
    void Draw(SkCanvas&) const override;
    void VisitOptions(const OptionsVisitor&) const override;
    std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override;

    // Reports 1 but if the object is iconified, it checks the CoarseBounds()
    // and returns a scale that would fit in a 1x1cm square.
    float GetBaseScale() const override;

    // Returns connection points on the sides and on top of the object's CoarseBounds().
    void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const override;

    // Returns the start position of the given argument.
    Vec2AndDir ArgStart(const Argument&) override;

    // When iconified, prevent children from receiving pointer events.
    bool AllowChildPointerEvents(ui::Widget&) const override;

    // Shortcut for automat::IsIconified(Object*).
    bool IsIconified() const;

    template <typename T>
    Ptr<T> LockObject() const {
      return object.Lock().Cast<T>();
    }
  };

  // Find or create a widget for this object, under the given parent.
  WidgetInterface& FindWidget(ui::Widget* parent);

  virtual std::unique_ptr<WidgetInterface> MakeWidget(ui::Widget* parent) {
    if (auto w = dynamic_cast<WidgetInterface*>(this)) {
      // Many legacy objects (Object/Widget hybrids) don't properly set their `object` field.
      if (auto widget_base = dynamic_cast<WidgetBase*>(this)) {
        widget_base->object = AcquireWeakPtr();
      }
      // Proxy object that can be lifetime-managed by the UI infrastructure (without
      // affecting the original object's lifetime).
      struct HybridAdapter : WidgetBase {
        Ptr<Object> ptr;
        WidgetInterface& widget;
        HybridAdapter(Widget* parent, Object& obj, WidgetInterface& widget)
            : WidgetBase(parent), ptr(obj.AcquirePtr()), widget(widget) {
          widget.parent = this;
          object = ptr;
        }
        StrView Name() const override { return "HybridAdapter"; }
        SkPath Shape() const override { return widget.Shape(); }
        Optional<Rect> TextureBounds() const override { return std::nullopt; }
        bool CenteredAtZero() const override { return widget.CenteredAtZero(); }
        void FillChildren(Vec<Widget*>& children) override { children.push_back(&widget); }
        void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const override {
          widget.ConnectionPositions(out_positions);
        }
        void Draw(SkCanvas& canvas) const override {
          // We don't want the default green fill for the object.
          Widget::Draw(canvas);
        }

        float GetBaseScale() const override { return widget.GetBaseScale(); }
      };
      return std::make_unique<HybridAdapter>(parent, *this, *w);
    }
    auto w = std::make_unique<WidgetBase>(parent);
    w->object = AcquireWeakPtr();
    return w;
  }

  void ForEachWidget(std::function<void(ui::RootWidget&, ui::Widget&)> cb);

  void WakeWidgetsAnimation();

  // Used to report errors within this object. If an error was caused by some other
  // "error reporter", take a look at ReportError in error.hh.
  void ReportError(std::string_view message,
                   std::source_location location = std::source_location::current());

  // Clears the error reported by the object itself
  void ClearOwnError();

  // If this object is owned by a Machine, return the Location that's used to store it.
  Location* MyLocation();
};

// MemberPtr holds a strong reference to an Object and a pointer to one of its Named members.
// This is useful for pointing to interfaces, fields, or other named components within an object.
struct MemberPtr : NestedPtr<Named> {
  using Base = NestedPtr<Named>;

  MemberPtr() : Base() {}

  // Construct from an Object and a pointer to one of its members
  MemberPtr(Ptr<Object>&& owner, Named* member)
      : Base(std::move(owner).Cast<ReferenceCounted>(), member) {}

  MemberPtr(const Ptr<Object>& owner, Named* member) : Base(Ptr<ReferenceCounted>(owner), member) {}

  // Construct from base type
  MemberPtr(Base&& base) : Base(std::move(base)) {}
  MemberPtr(const Base& base) : Base(base) {}

  // Allow conversion from NestedPtr<T> where T derives from Named
  template <typename T>
    requires std::convertible_to<T*, Named*>
  MemberPtr(NestedPtr<T>&& other) : Base(std::move(other)) {}

  template <typename T>
    requires std::convertible_to<T*, Named*>
  MemberPtr(const NestedPtr<T>& other) : Base(other) {}

  // Assignment operators
  MemberPtr& operator=(const MemberPtr& other) {
    Base::operator=(other);
    return *this;
  }

  MemberPtr& operator=(MemberPtr&& other) {
    Base::operator=(std::move(other));
    return *this;
  }

  // Get the owning Object
  Object* GetObject() {
    return GetOwner<Object>();  // uses static_cast, owner is always an Object
  }

  // Dynamic cast the member to a specific type
  template <typename T>
  T* As() const {
    return dynamic_cast<T*>(Get());
  }
};

// MemberWeakPtr holds a weak reference to an Object and a pointer to one of its Named members.
struct MemberWeakPtr : NestedWeakPtr<Named> {
  using Base = NestedWeakPtr<Named>;

  MemberWeakPtr() : Base() {}
  MemberWeakPtr(const MemberWeakPtr& ptr) : Base(ptr) {}

  // Construct from a MemberPtr
  MemberWeakPtr(const MemberPtr& ptr) : Base(ptr) {}

  // Construct from an Object's weak pointer and a member pointer
  MemberWeakPtr(WeakPtr<Object>&& owner, Named* member) : Base(std::move(owner), member) {}
  MemberWeakPtr(const WeakPtr<Object>& owner, Named* member) : Base(owner, member) {}

  // Construct from base type
  MemberWeakPtr(Base&& base) : Base(std::move(base)) {}
  MemberWeakPtr(const Base& base) : Base(base) {}

  // Allow conversion from NestedWeakPtr<T> where T derives from Named
  template <typename T>
    requires std::convertible_to<T*, Named*>
  MemberWeakPtr(const NestedWeakPtr<T>& other)
      : Base(other.GetOwnerWeak(), other.GetValueUnsafe()) {}

  // Assignment operators
  MemberWeakPtr& operator=(const MemberWeakPtr& other) {
    Base::operator=(other);
    return *this;
  }

  MemberWeakPtr& operator=(MemberWeakPtr&& other) {
    Base::operator=(std::move(other));
    return *this;
  }

  // Lock to get a MemberPtr
  MemberPtr Lock() const { return MemberPtr(Base::Lock()); }

  // Get the owning Object (unsafe - may be expired)
  Object* GetObjectUnsafe() const {
    if (auto owner = GetOwnerWeak().Lock()) {
      return static_cast<Object*>(owner.Get());
    }
    return nullptr;
  }

  // Dynamic cast the member to a specific type (unsafe - doesn't check if owner is alive)
  template <typename T>
  T* AsUnsafe() const {
    return dynamic_cast<T*>(GetValueUnsafe());
  }
};

}  // namespace automat
