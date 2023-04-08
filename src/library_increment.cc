#include "library_increment.h"

#include "library_macros.h"
#include "library_number.h"

namespace automaton::library {

DEFINE_PROTO(Increment);

Argument Increment::target_arg =
    Argument("target", Argument::kRequiresConcreteType)
        .RequireInstanceOf<Number>();

string_view Increment::Name() const { return "Increment"; }

std::unique_ptr<Object> Increment::Clone() const {
  return std::make_unique<Increment>();
}

void Increment::Run(Location &h) {
  auto integer = target_arg.GetTyped<Number>(h);
  if (!integer.ok) {
    return;
  }
  integer.typed->value += 1;
  integer.location->ScheduleUpdate();
}

} // namespace automaton::library