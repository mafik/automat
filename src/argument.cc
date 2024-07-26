#include "argument.hh"

#include "base.hh"
#include "svg.hh"

namespace automat {

Argument next_arg("next", Argument::kOptional);

Argument::FinalLocationResult Argument::GetFinalLocation(
    Location& here, std::source_location source_location) const {
  FinalLocationResult result(GetObject(here, source_location));
  if (auto live_object = dynamic_cast<LiveObject*>(result.object)) {
    result.final_location = live_object->here;
  }
  return result;
}

Argument::LocationResult Argument::GetLocation(Location& here,
                                               std::source_location source_location) const {
  LocationResult result;
  auto conn_it = here.outgoing.find(name);
  if (conn_it != here.outgoing.end()) {  // explicit connection
    auto c = conn_it->second;
    result.location = &c->to;
    result.follow_pointers = c->pointer_behavior == Connection::kFollowPointers;
  } else {  // otherwise, search for other locations in this machine
    result.location = reinterpret_cast<Location*>(here.Nearby([&](Location& other) -> void* {
      if (other.name == name) {
        return &other;
      }
      return nullptr;
    }));
  }
  if (result.location == nullptr && precondition >= kRequiresLocation) {
    here.ReportError(
        f("The %s argument of %s is not connected.", name.c_str(), here.ToStr().c_str()),
        source_location);
    result.ok = false;
  }
  return result;
}

Argument::ObjectResult Argument::GetObject(Location& here,
                                           std::source_location source_location) const {
  ObjectResult result(GetLocation(here, source_location));
  if (result.location) {
    if (result.follow_pointers) {
      result.object = result.location->Follow();
    } else {
      result.object = result.location->object.get();
    }
    if (result.object == nullptr && precondition >= kRequiresObject) {
      here.ReportError(f("The %s argument of %s is empty.", name.c_str(), here.ToStr().c_str()),
                       source_location);
      result.ok = false;
    }
  }
  return result;
}

struct DrawableSkPath : PaintDrawable {
  SkPath path;
  DrawableSkPath(SkPath path) : path(std::move(path)) {}
  SkRect onGetBounds() override { return path.getBounds(); }
  void onDraw(SkCanvas* c) override { c->drawPath(path, paint); }
};

PaintDrawable& Argument::Icon() {
  static DrawableSkPath default_icon = []() {
    SkPath path = PathFromSVG(kNextShape);
    return DrawableSkPath(path);
  }();
  return default_icon;
}
bool Argument::IsOn(Location& here) const {
  if (field) {
    if (auto on_off = dynamic_cast<OnOff*>(field)) {
      return on_off->IsOn();
    }
  }
  return false;
}

#pragma region New API

Object* Argument::FindObject(Location& here, IfMissing if_missing) const {
  auto conn_it = here.outgoing.find(name);
  Object* result = nullptr;
  if (conn_it != here.outgoing.end()) {  // explicit connection
    auto c = conn_it->second;
    result = c->to.object.get();
  } else {  // otherwise, search for other locations in this machine
    result = reinterpret_cast<Object*>(here.Nearby([&](Location& other) -> void* {
      if (Length(other.position - here.position) < search_radius) {
        for (auto& req : requirements) {
          std::string error;
          req(&other, other.object.get(), error);
          if (!error.empty()) {
            return nullptr;
          }
        }
        return other.object.get();
      }
      return nullptr;
    }));
  }

  if (result == nullptr && if_missing == IfMissing::CreateFromPrototype) {
    // Ask the current location for the prototype for this object.
    if (auto prototype = here.object->ArgPrototype(*this)) {
      if (auto machine = here.ParentAs<Machine>()) {
        Location& l = machine->Create(*prototype);
        result = l.object.get();
        Rect object_bounds = result->Shape().getBounds();
        l.position = here.position + Rect::BottomCenter(here.object->Shape().getBounds()) -
                     object_bounds.TopCenter();
        PositionBelow(here, l);
        AnimateGrowFrom(here, l);
      }
    }
  }
  return result;
}

}  // namespace automat