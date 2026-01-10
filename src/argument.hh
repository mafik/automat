// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>

#include "color.hh"
#include "drawable.hh"
#include "location.hh"
#include "optional.hh"
#include "status.hh"

namespace automat {

enum class CableTexture {
  Smooth,
  Braided,
};

// Arguments are responsible for finding dependencies (input & output) of objects.
// - they know about the requirements of the target object
// - they can connect fields of source objects (rather than whole objects)
// - they may automatically create target objects using given prototype
// - they may search for nearby valid objects in a given search radius
//
// IMPORTANT: Arguments are identified by their ADDRESS in memory (not name!). Don't move them
// around!
//
// TODO: think about pointer following
// TODO: think about requirement checking
// TODO: think about multiple arguments
struct Argument : virtual Part {
  enum class Style { Arrow, Cable, Spotlight, Invisible };

  virtual SkColor Tint() const { return "#404040"_color; }
  virtual SkColor Light() const { return "#ef9f37"_color; }
  virtual float AutoconnectRadius() const { return 0_cm; }
  virtual Style GetStyle() const { return Style::Arrow; }

  // Uncomment to find potential bugs related to arguments being moved around.
  // Argument(const Argument&) = delete;
  // Argument& operator=(const Argument&) = delete;
  // Argument(Argument&&) = delete;

  virtual ~Argument() = default;

  virtual void CanConnect(Object& start, Part& end, Status& status) const {
    AppendErrorMessage(status) += "Argument::CanConnect should be overridden";
  }

  bool CanConnect(Object& start, Part& end) const {
    Status status;
    CanConnect(start, end, status);
    return OK(status);
  }

  // This function should register a connection from `start` to the `end` so that subsequent calls
  // to `Find` will return `end`.
  //
  // When `end` is nullptr, this disconnects the existing connection. The implementation can check
  // the current connection value before clearing it (e.g., to call cleanup methods).
  virtual void Connect(Object& start, const NestedPtr<Part>& end) = 0;

  void Disconnect(Object& start) { Connect(start, {}); }

  virtual NestedPtr<Part> Find(Object& start) const = 0;

  // Returns the prototype object for this argument, or nullptr if there is no prototype.
  // This is used when creating new objects from arguments.
  virtual Ptr<Object> Prototype() const { return nullptr; }

  virtual PaintDrawable& Icon();  // TODO: weird - clean this up

  enum class IfMissing { ReturnNull, CreateFromPrototype };

  struct FindConfig {
    IfMissing if_missing = IfMissing::ReturnNull;
  };

  // Return the position and direction of this argument in the given Widget's coordinate
  // space.
  //
  // If the passed widget is the RootWidget, then the returned position will use the root coordinate
  // space (pixels), but it's not the only option. The widget could also be located at some
  // intermediate level so the returned position will be located within some parent object (for
  // example, a Machine).
  Vec2AndDir Start(ui::Widget& object_widget, ui::Widget& coordinate_space) const;

  // The returned "to_points" use the target object's local coordinate space.
  void NearbyCandidates(Location& here, float radius,
                        std::function<void(Location&, Vec<Vec2AndDir>& to_points)> callback) const;

  // TODO: remove this
  Location* FindLocation(Location& here, const FindConfig&) const;

  Object* FindObject(Location& here, const FindConfig&) const;

  template <typename T>
  T* FindObject(Location& here, const FindConfig& cfg) const {
    return dynamic_cast<T*>(FindObject(here, cfg));
  }
};

struct InlineArgument : Argument {
  NestedWeakPtr<Part> end;

  void Connect(Object&, const NestedPtr<Part>& end) override { this->end = end; }

  NestedPtr<Part> Find(Object&) const override { return end.Lock(); };
};

struct NextArg : Argument {
  StrView Name() const override { return "next"sv; }
  void CanConnect(Object& start, Part& end, Status&) const override;
  void Connect(Object& start, const NestedPtr<Part>& end) override;
  NestedPtr<Part> Find(Object& start) const override;
};

extern NextArg next_arg;

}  // namespace automat
