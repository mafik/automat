export module error;

import <string>;
import <source_location>;

export namespace automaton {

struct Error {
  std::string text;
  std::source_location location;
  Error(std::string_view text, std::source_location location = std::source_location::current()) : text(text), location(location) {}
};

inline std::ostream &operator<<(std::ostream &os, const Error &e) {
  return os << e.text;
}

} // namespace automaton
