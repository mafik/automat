#pragma once

#include <memory>
#include <source_location>
#include <string_view>

#include "math.hh"
#include "time.hh"

// Functions for logging human-readable messages.
//
// Usage:
//
//   LOG << "regular message";
//   ERROR << "error message";
//   FATAL << "stop the execution / print stack trace";
//
// Logging can also accept other types - integers & floats. When executed with
// HYPERDECK_SERVER defined, it will also accept a Client instance:
//
//   LOG << client << "Client connected";
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

enum LogLevel { LOG_LEVEL_DISCARD, LOG_LEVEL_INFO, LOG_LEVEL_ERROR, LOG_LEVEL_FATAL };

struct Logger {
  Logger(LogLevel, const std::source_location location = std::source_location::current());
  ~Logger();
  struct Impl;
  Impl* impl;
};

struct LOG : public Logger {
  LOG(const std::source_location location = std::source_location::current())
      : Logger(LOG_LEVEL_INFO, location) {}
};

struct ERROR : public Logger {
  ERROR(const std::source_location location = std::source_location::current())
      : Logger(LOG_LEVEL_ERROR, location) {}
};

struct FATAL : public Logger {
  FATAL(const std::source_location location = std::source_location::current())
      : Logger(LOG_LEVEL_FATAL, location) {}
};

const Logger& operator<<(const Logger&, int);
const Logger& operator<<(const Logger&, unsigned);
const Logger& operator<<(const Logger&, unsigned long);
const Logger& operator<<(const Logger&, unsigned long long);
const Logger& operator<<(const Logger&, float);
const Logger& operator<<(const Logger&, double);
const Logger& operator<<(const Logger&, std::string_view);
const Logger& operator<<(const Logger&, const unsigned char*);

// Support for logging vec's from math.hh
const Logger& operator<<(const Logger&, Vec2);
const Logger& operator<<(const Logger&, Vec3);

template <typename T>
concept loggable = requires(T& v) {
  { v.LoggableString() } -> std::convertible_to<std::string_view>;
};

const Logger& operator<<(const Logger& logger, loggable auto& t) {
  return logger << t.LoggableString();
}

void LOG_Indent(int n = 2);

void LOG_Unindent(int n = 2);

#define EVERY_N_SEC(n)              \
  static time::point last_log_time; \
  if (time::now() - last_log_time > time::duration(n) ? (last_log_time = time::now(), true) : false)

// TODO: remove
#define LOG_EVERY_N_SEC(n)                                     \
  static time::point last_log_time;                            \
  (time::now() - last_log_time > time::duration(n)             \
       ? (last_log_time = time::now(), Logger(LOG_LEVEL_INFO)) \
       : Logger(LOG_LEVEL_DISCARD))

// End of header
