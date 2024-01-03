#pragma once

#include <memory>
#include <source_location>

#include "str.hh"

namespace maf {

struct Status {
  struct Entry {
    std::unique_ptr<Entry> next;
    std::source_location location;
    Str message;
    Str advice;
  };

  std::unique_ptr<Entry> entry;

  int errsv; // Saved errno value

  Status();

  Str &operator()(const std::source_location location_arg =
                      std::source_location::current());

  bool Ok() const;
  Str ToStr() const;
  void Reset();
} __attribute__((packed));

inline bool OK(const Status &status) { return status.Ok(); }
inline Str ErrorMessage(const Status &s) { return s.ToStr(); }
inline Str &AppendErrorMessage(
    Status &status,
    const std::source_location location_arg = std::source_location::current()) {
  return status(location_arg);
}
void AppendErrorAdvice(Status &, StrView advice);

#define RETURN_ON_ERROR(status)                                                \
  if (!OK(status)) {                                                           \
    AppendErrorMessage(status) += __FUNCTION__;                                \
    return;                                                                    \
  }

#define RETURN_VAL_ON_ERROR(status, value)                                     \
  if (!OK(status)) {                                                           \
    AppendErrorMessage(status) += __FUNCTION__;                                \
    return value;                                                              \
  }

} // namespace maf