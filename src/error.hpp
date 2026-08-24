#pragma once
// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT

#include <source_location>
#include <string>

#include "fn.hpp"
#include "ptr.hpp"

namespace automat {

struct Object;

/*

# Goals

1. Automat should continue working even when some parts of it fail.
2. Error recovery should be not only possible but fun.

# Design

Errors can be attached to VM Objects. Each Object can have up to one Error. Attaching (reporting) an
error makes the Object enter a "failed" state. Objects can enter this state explicitly (by calling
ReportError) or due to signal being delivered.

Objects that do work should check for the failed state and not do anything until it's cleared.

Object that use ReportError to report errors on themselves on other objects are also responsible for
automatically clearing detected errors. If automatic clearing fails, the "failed" state can also be
cleared explicitly by the user (TODO).

Objects in the "failed" state burn and display a human-readable error message.

Errors are not propagated by default (everything else keeps running) but may be propagated if user
makes it explicit. This default makes the system more robust.

Error information is stored by the VM Object and triggers a regular Wakeup when it's changed.
Default implementation stores errors out of band but Objects should be able to store them in-line as
well.

TODO: Data loss prevention: allow Errors to store information that can be accessed by the user.

# Open questions

1. Better way to store Error information
  (currently it's some bizarre mutex-protected vector)

# Important files

error_recovery.hpp - converts fatal errors into LowLevelError exception
error_flames.hpp - visual error indicator

*/
struct Error {
  WeakPtr<Object> target = nullptr;    // target is the object that "burns"
  WeakPtr<Object> reporter = nullptr;  // reporter is responsible for clearing the error
  std::string text = "";
  std::source_location source_location = {};

  Error();
  ~Error();

  bool IsPresent() const { return reporter != nullptr; }
  void Clear() { reporter.Reset(); }
};

inline std::ostream& operator<<(std::ostream& os, const Error& e) { return os << e.text; }

// TODO: Error watching
// struct ErrorWatcher {
//   virtual ~ErrorWatcher() = default;

//   virtual void OnErrored(Ptr<Object> errored) = 0;
// };

// struct ErrorWatch {
//   ErrorWatcher& watcher;
//   WeakPtr<Object> target;

//   void Release();
// };

// ErrorWatch& WatchErrors(ErrorWatcher&, Object& target);

bool HasError(Object& target, Fn<void(Error&)> use_error = nullptr);

// Clear error caused by the given reporter
void ClearError(Object& target, Object& reporter);

// Mid-level helper for reporting errors. It allows the reported errors to have
// a `reporter` different from the `target` object.
void ReportError(Object& target, Object& reporter, std::string_view message,
                 std::source_location location = std::source_location::current());

// Low-level function for manipulating errors in a thread-safe way.
//
// It's mostly used internally. If it's used by other modules then it might make
// sense to provide a higher-level helper.
void ManipulateError(Object& target, Fn<void(Error&)> manip_error);

}  // namespace automat
