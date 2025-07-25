// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#pragma once

// Functions for logging human-readable messages.
//
// Usage:
//
//   LOG << "regular message";
//   ERROR << "error message";
//   FATAL << "stop the execution / print stack trace";
//
// Logging can also accept other types - integers & floats.
//
// In this case a short client identifier (IP address + 4-digit hash) will be
// printed before the massage. Each client will also get a random color to make
// identification easier.
//
// When executed within Emscripten, logging causes the messages to appear in the
// JavaScript console - as regular (black) messages (LOG), yellow warnings
// (ERROR) & red errors (FATAL).
//
// Logged messages can have multiple lines - the extra lines are not indented or
// treated in any special way.
//
// There is no need to add a new line character at the end of the logged message
// - it's added there automatically.

#include <chrono>
#include <functional>
#include <source_location>

#include "status.hh"
#include "str.hh"

namespace automat {

enum class LogLevel { Ignore, Info, Error, Fatal };

// Appends the logged message when destroyed.
struct LogEntry {
  LogLevel log_level;
  std::chrono::system_clock::time_point timestamp;
  std::source_location location;
  mutable std::string buffer;
  mutable int errsv;  // saved errno (if any)

  LogEntry(LogLevel, const std::source_location location = std::source_location::current());
  ~LogEntry();
};

using Logger = std::function<void(const LogEntry&)>;

void DefaultLogger(const LogEntry& e);

// The default logger prints to stdout (or JavaScript console when running under
// Emscripten).
std::vector<Logger>& GetLoggers();

#define LOG LogEntry(LogLevel::Info, std::source_location::current())

#define ERROR LogEntry(LogLevel::Error, std::source_location::current())
#define FATAL LogEntry(LogLevel::Fatal, std::source_location::current())

#define ERROR_ONCE                                                                             \
  static bool LOG_##__COUNTER__ = true;                                                        \
  automat::LogEntry((LOG_##__COUNTER__ ? (LOG_##__COUNTER__ = false, automat::LogLevel::Error) \
                                       : automat::LogLevel::Ignore),                           \
                    std::source_location::current())

const LogEntry& operator<<(const LogEntry&, StrView);
const LogEntry& operator<<(const LogEntry&, Status& status);

const LogEntry& operator<<(const LogEntry& logger, const Stringer auto& t) {
  return logger << ToStr(t);
}

void LOG_Indent(int n = 2);

void LOG_Unindent(int n = 2);

struct LOG_IndentGuard {
  LOG_IndentGuard(int n = 2) : n(n) { LOG_Indent(n); }
  ~LOG_IndentGuard() { LOG_Unindent(n); }

  int n;
};

#define EVERY_N_SEC(n)                                                                          \
  static std::chrono::steady_clock::time_point last_log_time;                                   \
  if (std::chrono::steady_clock::now() - last_log_time > std::chrono::steady_clock::duration(n) \
          ? (last_log_time = std::chrono::steady_clock::now(), true)                            \
          : false)

}  // namespace automat