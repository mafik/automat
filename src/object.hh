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

  virtual void ConnectionAdded(Location& here, Connection&) {}
  virtual void ConnectionRemoved(Location& here, Connection&) {}

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

  virtual OnOff* AsOnOff() { return nullptr; }

  virtual void Fields(std::function<void(Object&)> cb) {}

  virtual void Args(std::function<void(Argument&)> cb) {}

  virtual Ptr<Object> ArgPrototype(const Argument&) { return nullptr; }

  virtual void Updated(Location& here, Location& updated);

  virtual audio::Sound& NextSound();

  virtual std::partial_ordering operator<=>(const Object& other) const noexcept {
    return GetText() <=> other.GetText();
  }

  // The name for objects of this type. English proper noun, UTF-8, capitalized.
  // For example: "Text Editor".
  virtual std::string_view Name() const {
    const std::type_info& info = typeid(*this);
    return CleanTypeName(info.name());
  }

  struct WidgetInterface : ui::Widget {
    using ui::Widget::Widget;

    // Get the default scale that this object would like to have.
    // Usually it's 1 but when it's iconified, it may want to shrink itself.
    virtual float GetBaseScale() const = 0;

    // Places where the connections to this widget may terminate.
    // Local (metric) coordinates.
    virtual void ConnectionPositions(Vec<Vec2AndDir>& out_positions) const = 0;
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
  WidgetInterface& FindWidget(const ui::Widget* parent);

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
};

}  // namespace automat
