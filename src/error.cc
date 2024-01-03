#include "error.hh"

#include "object.hh"

namespace automat {

Error::~Error() {}

Error::Error(std::string_view text, std::source_location source_location)
    : text(text), source(nullptr), saved_object(nullptr), source_location(source_location) {}
}  // namespace automat