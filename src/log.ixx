export module log;

import <memory>;
import <source_location>;
import <string_view>;
import math;

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

enum LogLevel { LOG_LEVEL_INFO, LOG_LEVEL_ERROR, LOG_LEVEL_FATAL };

export struct Logger {
  Logger(LogLevel, const std::source_location location = std::source_location::current());
  ~Logger();
  struct Impl;
  Impl *impl;
};

export struct LOG : public Logger {
  LOG(const std::source_location location = std::source_location::current())
      : Logger(LOG_LEVEL_INFO, location) {}
};

export struct ERROR : public Logger {
  ERROR(const std::source_location location = std::source_location::current())
      : Logger(LOG_LEVEL_ERROR, location) {}
};

export struct FATAL : public Logger {
  FATAL(const std::source_location location = std::source_location::current())
      : Logger(LOG_LEVEL_FATAL, location) {}
};

export const Logger &operator<<(const Logger &, int);
export const Logger &operator<<(const Logger &, unsigned);
export const Logger &operator<<(const Logger &, unsigned long);
export const Logger &operator<<(const Logger &, unsigned long long);
export const Logger &operator<<(const Logger &, float);
export const Logger &operator<<(const Logger &, double);
export const Logger &operator<<(const Logger &, std::string_view);
export const Logger &operator<<(const Logger &, const unsigned char *);

// Support for logging vec's from math.hh
export const Logger &operator<<(const Logger &, vec2);
export const Logger &operator<<(const Logger &, vec3);

template <typename T>
concept loggable = requires(T &v) {
  { v.LoggableString() } -> std::convertible_to<std::string_view>;
};

export const Logger &operator<<(const Logger &logger, loggable auto &t) {
  return logger << t.LoggableString();
}

export void LOG_Indent(int n = 2);

export void LOG_Unindent(int n = 2);

// End of header
