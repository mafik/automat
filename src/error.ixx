export module error;

import <string>;
import <source_location>;

export namespace automaton {

/*

The goal of Errors is to explain to the user what went wrong and help with
recovery.

Errors can be placed in Locations (alongside Objects). Each location can hold up
to one Error.

While present, Errors pause the execution at their locations. Each object is
responsible for checking the error at its location and taking it into account
when executing itself.

Errors keep track of their source (object? location?) which is usually the same
as their location. Some objects can trigger errors at remote locations to pause
them.

Errors can be cleaned by the user or by their source. The source of the error
should clean it automatically - but sometimes it can be executed explicitly to
recheck conditions & clean the error. Errors caused by failing preconditions
clear themselves automatically when an object is executed.

Errors can also save objects that would otherwise be deleted. The objects are
held in the Error instance and can be accessed by the user.

In the UI the errors are visualized as spiders sitting on the error locations.
Source of the error is indicated by a spider web. Saved objects are cocoons.

When an error is added to an object it causes a notification to be sent to all
`error_observers` of the object. The observers may fix the error or notify the
user somehow. The parent Machine is an implicit error observer and propagates
the error upwards. Top-level Machines print their errors to the console.

TODO: Object::Errored should propagate the error to the parent Machine
TODO: keep track of the source of the error
TODO: maintain cocoon objects
TODO: add a new pointer type for "parent Machine" (use it in HealthTest)
TODO: new "Error Eater" object - deletes any errors as soon as they're reported

*/

struct Error {
  std::string text;
  std::source_location location;
  Error(std::string_view text,
        std::source_location location = std::source_location::current())
      : text(text), location(location) {}
};

inline std::ostream &operator<<(std::ostream &os, const Error &e) {
  return os << e.text;
}

} // namespace automaton
