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
  virtual ~Object() = default;

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
    return CleanTypeName(info.name());
  }

  struct WidgetInterface : ui::Widget {
    using ui::Widget::Widget;

    // Objects in Automat can be fairly large. Iconification is a mechanism that allows players to
    // shrink them so that they fit in a 1x1cm square.
    virtual bool IsIconified() const = 0;
    virtual void SetIconified(bool iconified) = 0;

    // Get the default scale that this object would like to have.
    // Usually it's 1 but when it's iconified, it may want to shrink itself.
    //
    // Default implementation reports 1 but if the object is iconified, it checks the CoarseBounds()
    // and returns a scale that would fit in a 1x1cm square.
    virtual float GetBaseScale() const;
  };

  // Provides sensible defaults for most object widgets. Designed to be inherited and tweaked.
  //
  // It's rendered as a green box with the name of the object.
  struct WidgetBase : WidgetInterface {
    WeakPtr<Object> object;
    bool iconified;

    WidgetBase(Widget* parent) : WidgetInterface(parent), iconified(false) {}

    std::string_view Name() const override;
    virtual float Width() const;
    virtual std::string Text() const { return std::string(Name()); }
    SkPath Shape() const override;
    void Draw(SkCanvas&) const override;
    void VisitOptions(const OptionsVisitor&) const override;
    std::unique_ptr<Action> FindAction(ui::Pointer& p, ui::ActionTrigger btn) override;

    template <typename T>
    Ptr<T> LockObject() const {
      return object.Lock().Cast<T>();
    }

    bool IsIconified() const override { return iconified; }
    void SetIconified(bool new_iconified) override { this->iconified = new_iconified; }
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

        bool IsIconified() const override { return widget.IsIconified(); }
        void SetIconified(bool new_iconified) override { widget.SetIconified(new_iconified); }
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
};

}  // namespace automat
