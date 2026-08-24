#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <memory>

#include "source_location.hpp"
#include "str.hpp"

namespace automat {

enum StatusCode {
  STATUS_OK = 0,
  STATUS_FAILED = 1,
};

struct Status {
  struct Entry {
    std::unique_ptr<Entry> next;
    SourceLocation location;
    Str message;
    Str advice;
  };

  std::unique_ptr<Entry> entry;

  int errsv;  // Saved errno value

  Status();

  Str& operator()(SourceLocation location_arg = std::source_location::current());

  bool Ok() const;
  Str ToStr() const;

  // Clears the recorded error and sets the status to "OK".
  void Reset();
} __attribute__((packed));

inline bool OK(const Status& status) { return status.Ok(); }
inline Str ErrorMessage(const Status& s) { return s.ToStr(); }
inline Str& AppendErrorMessage(Status& status,
                               SourceLocation location_arg = std::source_location::current()) {
  return status(std::move(location_arg));
}
void AppendErrorAdvice(Status&, StrView advice);

#define RETURN_ON_ERROR(status)                 \
  if (!OK(status)) {                            \
    AppendErrorMessage(status) += __FUNCTION__; \
    return;                                     \
  }

#define RETURN_VAL_ON_ERROR(status, value)      \
  if (!OK(status)) {                            \
    AppendErrorMessage(status) += __FUNCTION__; \
    return value;                               \
  }

}  // namespace automat
