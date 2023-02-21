export module action;

import <string_view>;
import base;

namespace automaton {

using namespace std::literals;

// Generic base class for actions that can be invoked by the user or
// automatically.
struct Action : Object {
  // Returns a buffer with the icon contents
  virtual std::string_view Icon() const = 0;
  virtual std::string_view Name() const = 0;
  virtual std::string_view Accelerator() const { return ""sv; };
  virtual void Begin(Location *){};
  virtual void Update(Location *){};
  virtual void End(Location *){};
};

} // namespace automaton
