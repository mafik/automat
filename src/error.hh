// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

#include <source_location>
#include <string>

#include "fn.hh"
#include "ptr.hh"

namespace automat {

struct Object;

/*
The goal of Errors is to explain to the user what went wrong and help with
recovery.

Errors can be attached to Objects. Each Object can have up to one Error.

While present, Errors pause the execution of their objects. Each object is
responsible for checking its error and taking it into account when executing
itself.

Errors may be attached to objects by external "reporters". They work like
validators that can look for issues and attach the errors to stop the execution.
Errors keep track of their reporter (which is usually the same as their target).

Errors can be cleaned by the user or by their reporter. The reporter of the error
should clean it automatically - but sometimes it can be executed explicitly to
recheck conditions & clean the error. Errors caused by failing preconditions
clear themselves automatically when an object is executed.

TODO: Errors can also save objects that would otherwise be deleted. The objects
are held in the Error instance and can be accessed by the user.

In the UI the errors are visualized as fire with a smoke bubble explaining the
issue.

TODO: Visualize reporters

TODO: Visualize error messages

When an error is added to an object it causes a notification to be sent to all
`error_observers` of the object. The observers may fix the error or notify the
user somehow. The parent Machine is an implicit error observer and propagates
the error upwards. Top-level Machines print their errors to the console.
*/
struct Error {
  WeakPtr<Object> target = nullptr;    // target is the object that "burns"
  WeakPtr<Object> reporter = nullptr;  // reporter is the object that started the fire
  // TODO: Object saving
  // Ptr<Object> saved_object = nullptr;
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
void ClearError(Object& target);

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
