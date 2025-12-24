// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "color.hh"
#include "drawable.hh"
#include "format.hh"
#include "location.hh"
#include "optional.hh"

namespace automat {

enum class CableTexture {
  Smooth,
  Braided,
};

// Arguments are responsible for finding dependencies (input & output) of objects.
// - the know how to follow pointers (objects that point to other objects)
// - they know about the requirements of the target object
// - they can connect fields of source objects (rather than whole objects)
// - they may automatically create target objects using given prototype
// - they may search for nearby valid objects in a given search radius
//
// IMPORTANT: Arguments are identified by their ADDRESS in memory (not name!). Don't move them
// around!
//
// Note: the API for Arguments is in the process of being re-designed - see the bottom of this file
//
// TODO: finish re-design
// TODO: think about pointer following
// TODO: think about requirement checking
// TODO: think about multiple arguments
struct Argument {
  enum Precondition {
    kOptional,
    kRequiresLocation,
    kRequiresObject,
    kRequiresConcreteType,
  };
  // Used for annotating arguments so that (future) UI can show them
  // differently.
  enum Quantity {
    kSingle,
    kMultiple,
  };

  std::string name;
  Precondition precondition;
  Quantity quantity;
  std::vector<std::function<void(Location* location, Object* object, std::string& error)>>
      requirements;
  SkColor tint = "#404040"_color;
  SkColor light = "#ef9f37"_color;
  float autoconnect_radius = 0_cm;

  enum class Style { Arrow, Cable, Spotlight, Invisible } style = Style::Arrow;

  // TODO: get rid of this property, the parent should instead provide the "field" object based on
  // Argument.
  Interface* interface = nullptr;

  Argument(std::string_view name, Precondition precondition = kOptional,
           Quantity quantity = kSingle)
      : name(name), precondition(precondition), quantity(quantity) {}

  // Uncomment to find potential bugs related to arguments being moved around.
  // Argument(const Argument&) = delete;
  // Argument& operator=(const Argument&) = delete;
  // Argument(Argument&&) = delete;

  virtual ~Argument() = default;

  virtual PaintDrawable& Icon();
  virtual bool IsOn(Location& here) const;

  template <typename T>
  Argument& RequireInstanceOf() {
    requirements.emplace_back(
        [name = name](Location* location, Object* object, std::string& error) {
          if (dynamic_cast<T*>(object) == nullptr) {
            error = f("The {} argument must be an instance of {}.", name, typeid(T).name());
          }
        });
    return *this;
  }

  void CheckRequirements(Location& here, Location* location, Object* object, std::string& error) {
    for (auto& requirement : requirements) {
      requirement(location, object, error);
      if (!error.empty()) {
        return;
      }
    }
  }

  struct LocationResult {
    bool ok = true;
    bool follow_pointers = true;
    Location* location = nullptr;
  };

  LocationResult GetLocation(
      Location& here, std::source_location source_location = std::source_location::current()) const;

  struct ObjectResult : LocationResult {
    Object* object = nullptr;
    ObjectResult(LocationResult location_result) : LocationResult(location_result) {}
  };

  ObjectResult GetObject(
      Location& here, std::source_location source_location = std::source_location::current()) const;

  struct FinalLocationResult : ObjectResult {
    Location* final_location = nullptr;
    FinalLocationResult(ObjectResult object_result) : ObjectResult(object_result) {}
  };

  FinalLocationResult GetFinalLocation(
      Location& here, std::source_location source_location = std::source_location::current()) const;

  template <typename T>
  struct TypedResult : ObjectResult {
    T* typed = nullptr;
    TypedResult(ObjectResult object_result) : ObjectResult(object_result) {}
  };

  template <typename T>
  TypedResult<T> GetTyped(Location& here,
                          std::source_location source_location = std::source_location::current()) {
    TypedResult<T> result(GetObject(here, source_location));
    if (result.object) {
      result.typed = dynamic_cast<T*>(result.object);
      if (result.typed == nullptr && precondition >= kRequiresConcreteType) {
        ReportError(*here.object, *here.object,
                    f("The {} argument is not an instance of {}.", name, typeid(T).name()),
                    source_location);
        result.ok = false;
      }
    }
    return result;
  }

  // The Loop ends when `callback` returns a value that is convertible to
  // `true`.
  template <typename T>
  T LoopLocations(Location& here, std::function<T(Location&)> callback) {
    auto [begin, end] = here.outgoing.equal_range(this);
    for (auto it = begin; it != end; ++it) {
      auto conn = *it;
      if (auto ret = callback(conn->to)) {
        return ret;
      }
    }
    return T();
  }

  // The Loop ends when `callback` returns a value that is convertible to
  // `true`.
  template <typename T>
  T LoopObjects(Location& here, std::function<T(Object&)> callback) {
    return LoopLocations<T>(here, [&](Location& h) {
      if (Object* o = h.Follow()) {
        return callback(*o);
      } else {
        return T();
      }
    });
  }

  std::string DebugString() const {
    std::string ret = name;
    if (precondition == kOptional) {
      ret += " (optional)";
    } else if (precondition == kRequiresLocation) {
      ret += " (requires location)";
    } else if (precondition == kRequiresObject) {
      ret += " (requires object)";
    } else if (precondition == kRequiresConcreteType) {
      ret += " (requires concrete type)";
    }
    return ret;
  }

#pragma region New API
  ////////////////////////////////////////////////////////////////////
  // New, simple API - completely separate from the *Result APIs.
  ////////////////////////////////////////////////////////////////////

  enum class IfMissing { ReturnNull, CreateFromPrototype };

  struct FindConfig {
    IfMissing if_missing = IfMissing::ReturnNull;
    Optional<float> search_radius = std::nullopt;  // overrides autoconnect_radius
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

  Location* FindLocation(Location& here, const FindConfig&) const;

  Object* FindObject(Location& here, const FindConfig&) const;

  template <typename T>
  T* FindObject(Location& here, const FindConfig& cfg) const {
    return dynamic_cast<T*>(FindObject(here, cfg));
  }

  void InvalidateConnectionWidgets(Location& here) const;
};

extern Argument next_arg;

struct LiveArgument : Argument {
  LiveArgument(std::string_view name, Precondition precondition) : Argument(name, precondition) {}

  template <typename T>
  LiveArgument& RequireInstanceOf() {
    Argument::RequireInstanceOf<T>();
    return *this;
  }
  void Detach(Location& here);
  void Attach(Location& here);
  void Relocate(Location* old_here, Location* new_here) {
    if (old_here) {
      Detach(*old_here);
    }
    if (new_here) {
      Attach(*new_here);
    }
  }
  virtual void ConnectionAdded(Location& here, Connection& connection) {
    here.ObserveUpdates(connection.to);
    here.ScheduleLocalUpdate(connection.to);
  }
  virtual void ConnectionRemoved(Location& here, Connection& connection) {
    here.StopObservingUpdates(connection.to);
    here.ScheduleLocalUpdate(connection.to);
  }
  void Rename(Location& here, std::string_view new_name) {
    Detach(here);
    name = new_name;
    Attach(here);
  }
};

}  // namespace automat
