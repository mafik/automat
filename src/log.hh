#pragma once

#include <chrono>
#include <functional>
#include <source_location>
#include <string>
#include <string_view>

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
// Logged messages can have multiple lines - the extra lines are not indented or
// treated in any special way.
//
// There is no need to add a new line character at the end of the logged message
// - it's added there automatically.

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

// The default logger prints to stdout.
extern std::vector<Logger> loggers;

#define LOG LogEntry(LogLevel::Info, std::source_location::current())
#define ERROR LogEntry(LogLevel::Error, std::source_location::current())
#define FATAL LogEntry(LogLevel::Fatal, std::source_location::current())

const LogEntry& operator<<(const LogEntry&, int);
const LogEntry& operator<<(const LogEntry&, unsigned);
const LogEntry& operator<<(const LogEntry&, unsigned long);
const LogEntry& operator<<(const LogEntry&, unsigned long long);
const LogEntry& operator<<(const LogEntry&, float);
const LogEntry& operator<<(const LogEntry&, double);
const LogEntry& operator<<(const LogEntry&, std::string_view);
const LogEntry& operator<<(const LogEntry&, const unsigned char*);

template <typename T>
concept loggable = requires(T& v) {
  { v.LoggableString() } -> std::convertible_to<std::string_view>;
};

const LogEntry& operator<<(const LogEntry& logger, loggable auto& t) {
  return logger << t.LoggableString();
}

void LOG_Indent(int n = 2);

void LOG_Unindent(int n = 2);

#define EVERY_N_SEC(n)              \
  static time::point last_log_time; \
  if (time::now() - last_log_time > time::duration(n) ? (last_log_time = time::now(), true) : false)
