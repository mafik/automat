#pragma once

#include <string>
#include <string_view>

#include "widget.hh"

namespace automat {

struct Connection;
struct Location;
struct Pointer;
struct Argument;

// Objects are interactive pieces of data & behavior.
//
// Instances of this class provide custom logic & appearance.
struct Object : gui::Widget {
  // Create a copy of this object.
  //
  // Subclasses of Object should have a static `proto` field, holding their own
  // prototype instance. This prototype will be cloned when new objects are
  // created. See the `Machine::Create` function for details.
  virtual std::unique_ptr<Object> Clone() const = 0;

  virtual void Relocate(Location* new_here) {}
  virtual void ConnectionAdded(Location& here, std::string_view label, Connection& connection) {}

  // Release the memory occupied by this object.
  virtual ~Object() = default;

  virtual std::string GetText() const { return ""; }
  virtual void SetText(Location& error_context, std::string_view text) {}

  // Pointer-like objects can be followed.
  virtual Pointer* AsPointer() { return nullptr; }

  virtual void Args(std::function<void(Argument&)> cb) {}
  virtual SkPath ArgShape(Argument&) const { return SkPath(); }

  virtual void Run(Location& here) {}
  virtual void Updated(Location& here, Location& updated) { Run(here); }
  virtual void Errored(Location& here, Location& errored) {}

  virtual std::partial_ordering operator<=>(const Object& other) const noexcept {
    return GetText() <=> other.GetText();
  }
  void Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
};

template <typename T>
std::unique_ptr<Object> Create() {
  return T::proto.Clone();
}

}  // namespace automat