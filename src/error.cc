// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "error.hh"

#include "object.hh"

namespace automat {

Error::Error() {}
Error::~Error() {}

void ClearError(Object& target, Object& reporter) {
  ManipulateError(target, [&](Error& err) {
    if (err.reporter == &reporter) {
      err.Clear();
    }
  });
}

void ReportError(Object& target, Object& reporter, std::string_view message,
                 std::source_location location) {
  ManipulateError(target, [&](Error& err) {
    err.text = message;
    err.source_location = location;
    err.reporter = reporter.AcquireWeakPtr();
  });
}

Vec<Error> errors;
std::mutex errors_mutex;

static bool HasErrorUnsafe(Object& target, Fn<void(Error&)>& use_error) {
  for (int i = 0; i < errors.size(); ++i) {
    auto& err = errors[i];
    if (err.target == &target) {
      if (use_error == nullptr) {
        return true;
      }
      use_error(err);
      if (err.IsPresent()) {
        return true;
      } else {  // error was cleared - remove it from the vector
        if (i != errors.size() - 1) {
          err = std::move(errors.back());
        }
        errors.pop_back();
        return false;
      }
    }
  }
  return false;
}

bool HasError(Object& target, Fn<void(Error&)> use_error) {
  auto lock = std::lock_guard(errors_mutex);
  // LOG << "Checking for error in " << f("{}", (void*)&target);
  return HasErrorUnsafe(target, use_error);
}

void ManipulateError(Object& target, Fn<void(Error&)> manip_error) {
  auto lock = std::lock_guard(errors_mutex);
  if (HasErrorUnsafe(target, manip_error)) {
    return;
  }
  Error tmp{};
  manip_error(tmp);
  if (tmp.IsPresent()) {
    errors.emplace_back(std::move(tmp));
    errors.back().target = target.AcquireWeakPtr();
  }
}

}  // namespace automat
