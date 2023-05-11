#pragma once

#include "widget.h"

#include <string>
#include <string_view>

namespace automaton {

struct Connection;
struct Location;
struct Pointer;

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

  virtual void Relocate(Location *new_self) {}

  // Release the memory occupied by this object.
  virtual ~Object() = default;

  virtual std::string GetText() const { return ""; }
  virtual void SetText(Location &error_context, std::string_view text) {}
  // virtual void SetText(Location &error_context, string_view text) {
  //   auto error_message = std::format("{} doesn't support text input.",
  //   Name()); error_context.ReportError(error_message);
  // }

  // Pointer-like objects can be followed.
  virtual Pointer *AsPointer() { return nullptr; }

  virtual void ConnectionAdded(Location &here, std::string_view label,
                               Connection &connection) {}
  virtual void Run(Location &here) {}
  virtual void Updated(Location &here, Location &updated) { Run(here); }
  virtual void Errored(Location &here, Location &errored) {}
  virtual std::partial_ordering
  operator<=>(const Object &other) const noexcept {
    return GetText() <=> other.GetText();
  }
  void Draw(SkCanvas &canvas, animation::State &animation_state) const override;
  SkPath Shape() const override;
};

template <typename T> std::unique_ptr<Object> Create() {
  return T::proto.Clone();
}

} // namespace automaton