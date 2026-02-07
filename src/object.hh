// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "audio.hh"
#include "deserializer.hh"
#include "ptr.hh"
#include "string_multimap.hh"
#include "toy.hh"
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
struct Syncable;
struct ObjectSerializer;
struct ObjectDeserializer;

// Objects are interactive pieces of data & behavior.
//
// Instances of this class provide their logic.
// Appearance is delegated to Widgets.
struct Object : public ReferenceCounted, public ToyMaker {
  Location* here = nullptr;

  ReferenceCounted& GetOwner() override { return *this; }
  Atom& GetAtom() override { return *this; }

  Object() = default;

  // Create a copy of this object.
  //
  // Subclasses of Object should have a static `proto` field, holding their own
  // prototype instance. This prototype will be cloned when new objects are
  // created. See the `Machine::Create` function for details.
  virtual Ptr<Object> Clone() const = 0;

  virtual void Relocate(Location* new_here);

  // Release the memory occupied by this object.
  virtual ~Object();

  // # Atoms & Serialization
  //
  // Objects serialize themselves to the "value" property within the location objects
  // - Each atom my want to emit multiple properties

  virtual void SerializeState(ObjectSerializer& writer) const;

  // Deserializes a single field of the object. Returns true if the key was handled.
  // Called from persistence.cc for each key in the object's JSON representation.
  // Derived classes should override this to handle their own fields.
  virtual bool DeserializeKey(ObjectDeserializer& d, StrView key);

  virtual std::string GetText() const { return ""; }
  virtual void SetText(std::string_view text) {}

  virtual Container* AsContainer() { return nullptr; }

  // Image-like objects can provide image data.
  virtual ImageProvider* AsImageProvider() { return nullptr; }

  virtual LongRunning* AsLongRunning() { return nullptr; }

  virtual operator OnOff*() { return nullptr; }

  virtual void Atoms(const std::function<void(Atom&)>&);

  virtual void AtomName(Atom&, Str& out_name);

  virtual Atom* AtomFromName(StrView name);

  // Wrapper around Atoms() that only reports Arguments
  void Args(const std::function<void(Argument&)>&);

  virtual void Updated(WeakPtr<Object>& updated);

  virtual audio::Sound& NextSound();

  virtual std::partial_ordering operator<=>(const Object& other) const noexcept {
    return GetText() <=> other.GetText();
  }

  // Provides sensible defaults for most object widgets. Designed to be inherited and tweaked.
  //
  // It's rendered as a green box with the name of the object.
  struct WidgetBase : Toy {
    WeakPtr<Object> object;

    WidgetBase(Widget* parent, Object& object_ref)
        : Toy(parent), object(object_ref.AcquireWeakPtr()) {}

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

    // When iconified, prevent children from receiving pointer events.
    bool AllowChildPointerEvents(ui::Widget&) const override;

    // Shortcut for automat::IsIconified(Object*).
    bool IsIconified() const;

    template <typename T>
    Ptr<T> LockObject() const {
      return object.Lock().Cast<T>();
    }
  };

  std::unique_ptr<Toy> MakeToy(ui::Widget* parent) override {
    if (auto w = dynamic_cast<Toy*>(this)) {
      // Proxy object that can be lifetime-managed by the UI infrastructure (without
      // affecting the original object's lifetime).
      struct HybridAdapter : WidgetBase {
        Ptr<Object> ptr;
        Toy& widget;
        HybridAdapter(Widget* parent, Object& obj, Toy& widget)
            : WidgetBase(parent, obj), ptr(obj.AcquirePtr()), widget(widget) {
          widget.parent = this;
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
    // At the moment, Objects mix the memory management & their logic in one struct.
    // Long-term (after this is decoupled) all WidgetBase should be constructed with separate `rc`
    // control block.
    // Until this happens, we ignore the `rc` passed into MakeWidget & use the Object's built-in
    // ReferenceCounted base class.
    // See `Memory Management.md`.
    auto w = std::make_unique<WidgetBase>(parent, *this);
    return w;
  }

  void InvalidateConnectionWidgets(const Argument* arg = nullptr) const;

  // Used to report errors within this object. If an error was caused by some other
  // "error reporter", take a look at ReportError in error.hh.
  void ReportError(std::string_view message,
                   std::source_location location = std::source_location::current());

  // Clears the error reported by the object itself
  void ClearOwnError();

  // If this object is owned by a Machine, return the Location that's used to store it.
  Location* MyLocation();
};

struct ObjectSerializer : Serializer {
  using Serializer::Serializer;

  std::unordered_set<Str> assigned_names;
  std::unordered_map<Object*, Str> object_to_name;
  std::vector<Object*> serialization_queue;

  Str& ResolveName(Object&, StrView hint = ""sv);
  Str ResolveName(Object&, Atom*, StrView hint = ""sv);
  void Serialize(Object&);
};

struct ObjectDeserializer : Deserializer {
  string_map<Ptr<Object>> objects;

  using Deserializer::Deserializer;

  void RegisterObject(StrView name, Object& object);

  Object* LookupObject(StrView name);
  NestedPtr<Atom> LookupAtom(StrView name);
};

}  // namespace automat
