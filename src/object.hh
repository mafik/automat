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

// Objects are interactive pieces of data & behavior.
//
// Instances of this class provide custom logic & appearance.
struct Object : public virtual ReferenceCounted {
  Object() {}

  // Create a copy of this object.
  //
  // Subclasses of Object should have a static `proto` field, holding their own
  // prototype instance. This prototype will be cloned when new objects are
  // created. See the `Machine::Create` function for details.
  virtual Ptr<Object> Clone() const = 0;

  virtual void Relocate(Location* new_here) {}

  virtual void ConnectionAdded(Location& here, Connection&) {}
  virtual void ConnectionRemoved(Location& here, Connection&) {}

  // Release the memory occupied by this object.
  virtual ~Object() = default;

  virtual void SerializeState(Serializer& writer, const char* key = "value") const;

  // Restores state when Automat is restarted.
  virtual void DeserializeState(Location& l, Deserializer& d);

  virtual std::string GetText() const { return ""; }
  virtual void SetText(Location& error_context, std::string_view text) {}

  // Pointer-like objects can be followed.
  virtual Pointer* AsPointer() { return nullptr; }

  virtual Container* AsContainer() { return nullptr; }

  virtual void Fields(std::function<void(Object&)> cb) {}

  virtual void Args(std::function<void(Argument&)> cb) {}

  virtual Ptr<Object> ArgPrototype(const Argument&) { return nullptr; }

  virtual void Updated(Location& here, Location& updated);
  virtual void Errored(Location& here, Location& errored) {}

  virtual audio::Sound& NextSound();

  virtual std::partial_ordering operator<=>(const Object& other) const noexcept {
    return GetText() <=> other.GetText();
  }

  // The name for objects of this type. English proper noun, UTF-8, capitalized.
  // For example: "Text Editor".
  virtual std::string_view Name() const {
    const std::type_info& info = typeid(*this);
    return maf::CleanTypeName(info.name());
  }

  // Green box with a name of the object.
  struct FallbackWidget : gui::Widget {
    WeakPtr<Object> object;

    // FallbackWidget doesn't have a constructor that takes weak_ptr<Object> because it's sometimes
    // used as a base class for Object/Widget hybrids (which should be refactored BTW). When an
    // object is constructed, its weak_from_this is not available. It's only present after
    // construction finishes.

    std::string_view Name() const override;
    virtual float Width() const;
    virtual std::string Text() const { return std::string(Name()); }
    SkPath Shape() const override;
    void Draw(SkCanvas&) const override;
    void VisitOptions(const OptionsVisitor&) const override;
    std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override;

    template <typename T>
    Ptr<T> LockObject() const {
      return object.Lock().Cast<T>();
    }
  };

  virtual Ptr<gui::Widget> MakeWidget() {
    if (auto w = dynamic_cast<gui::Widget*>(this)) {
      // Many legacy objects (Object/Widget hybrids) don't properly set their `object` field.
      if (auto fallback_widget = dynamic_cast<FallbackWidget*>(this)) {
        fallback_widget->object = AcquireWeakPtr();
      }
      return w->AcquirePtr();
    }
    auto w = MakePtr<FallbackWidget>();
    w->object = AcquireWeakPtr();
    return w;
  }

  void ForEachWidget(std::function<void(gui::RootWidget&, gui::Widget&)> cb);

  void WakeWidgetsAnimation();
};

}  // namespace automat