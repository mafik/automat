// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

#include "audio.hh"
#include "deserializer.hh"
#include "math.hh"
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
  // TODO: separate objects & widgets!
  Object() {}

  // Create a copy of this object.
  //
  // Subclasses of Object should have a static `proto` field, holding their own
  // prototype instance. This prototype will be cloned when new objects are
  // created. See the `Machine::Create` function for details.
  virtual std::shared_ptr<Object> Clone() const = 0;

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

  virtual void Fields(std::function<void(Object&)> cb) {}

  virtual SkPath FieldShape(Object&) const { return SkPath(); }

  virtual void Args(std::function<void(Argument&)> cb) {}

  virtual Vec2AndDir ArgStart(const Argument&);
  virtual std::shared_ptr<Object> ArgPrototype(const Argument&) { return nullptr; }

  virtual void ConnectionPositions(maf::Vec<Vec2AndDir>& out_positions) const;

  virtual void Updated(Location& here, Location& updated);
  virtual void Errored(Location& here, Location& errored) {}

  virtual audio::Sound& NextSound();

  virtual std::partial_ordering operator<=>(const Object& other) const noexcept {
    return GetText() <=> other.GetText();
  }
  animation::Phase Draw(gui::DrawContext&) const override;
  SkPath Shape() const override;
  virtual RRect CoarseBounds() const;
  std::unique_ptr<Action> FindAction(gui::Pointer& p, gui::ActionTrigger btn) override;
};

template <typename T>
std::unique_ptr<Object> Create() {
  return T::proto.Clone();
}

}  // namespace automat