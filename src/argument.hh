#pragma once

#include <functional>
#include <string>
#include <vector>

#include "drawable.hh"
#include "format.hh"
#include "location.hh"

using namespace maf;

namespace automat {

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
  float search_radius = 20_cm;

  // TODO: get rid of this property, the parent should instead provide the "field" object based on
  // Argument.
  Object* field = nullptr;

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
            error = f("The %s argument must be an instance of %s.", name.c_str(), typeid(T).name());
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
        here.ReportError(
            f("The %s argument is not an instance of %s.", name.c_str(), typeid(T).name()),
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
    auto [begin, end] = here.outgoing.equal_range(name);
    for (auto it = begin; it != end; ++it) {
      if (auto ret = callback(it->second->to)) {
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

  Object* FindObject(Location& here, IfMissing = IfMissing::ReturnNull) const;

  template <typename T>
  T* FindObject(Location& here, IfMissing if_missing = IfMissing::ReturnNull) const {
    return dynamic_cast<T*>(FindObject(here, if_missing));
  }
};

extern Argument next_arg;

struct LiveArgument : Argument {
  LiveArgument(std::string_view name, Precondition precondition) : Argument(name, precondition) {}

  template <typename T>
  LiveArgument& RequireInstanceOf() {
    Argument::RequireInstanceOf<T>();
    return *this;
  }
  void Detach(Location& here) {
    auto connections = here.outgoing.equal_range(name);
    for (auto it = connections.first; it != connections.second; ++it) {
      auto& connection = it->second;
      here.StopObservingUpdates(connection->to);
    }
    // If there were no connections, try to find nearby objects instead.
    if (connections.first == here.outgoing.end()) {
      here.Nearby([&](Location& other) {
        if (other.name == name) {
          here.StopObservingUpdates(other);
        }
        return nullptr;
      });
    }
  }
  void Attach(Location& here) {
    auto connections = here.outgoing.equal_range(name);
    for (auto it = connections.first; it != connections.second; ++it) {
      auto& connection = it->second;
      here.ObserveUpdates(connection->to);
    }
    // If there were no connections, try to find nearby objects instead.
    if (connections.first == here.outgoing.end()) {
      here.Nearby([&](Location& other) {
        if (other.name == name) {
          here.ObserveUpdates(other);
        }
        return nullptr;
      });
    }
  }
  void Relocate(Location* old_self, Location* new_self) {
    if (old_self) {
      Detach(*old_self);
    }
    if (new_self) {
      Attach(*new_self);
    }
  }
  void ConnectionAdded(Location& here, std::string_view label, Connection& connection) {
    // TODO: handle the case where `here` is observing nearby objects (without
    // connections) and a connection is added.
    // TODO: handle ConnectionRemoved
    if (label == name) {
      here.ObserveUpdates(connection.to);
      here.ScheduleLocalUpdate(connection.to);
    }
  }
  void Rename(Location& here, std::string_view new_name) {
    Detach(here);
    name = new_name;
    Attach(here);
  }
};

}  // namespace automat