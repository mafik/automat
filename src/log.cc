#include "log.hh"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include "format.hh"
#include "math.hh"
#include "term.hh"

static int indent = 0;

void LOG_Indent(int n) { indent += n; }

void LOG_Unindent(int n) { indent -= n; }

struct Logger::Impl {
  std::string buffer;
  LogLevel log_level;
  std::source_location location;

  Impl(LogLevel log_level_arg, const std::source_location location_arg)
      : log_level(log_level_arg), location(location_arg) {
#if !defined(__EMSCRIPTEN__)
    // print time
    time_t timestamp = time(nullptr);
    char* t = asctime(localtime(&timestamp));
    char* h = t + 11;
    h[2] = 0;
    char* m = t + 14;
    m[2] = 0;
    char* s = t + 17;
    s[2] = 0;

    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    int64_t millis = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

    auto dark = term::Gray(12);
    auto darker = term::Gray(8);
    auto darkest = term::Gray(4);

    buffer += dark(h) + darkest(":") + dark(m) + darkest(":") + dark(s) + darkest(".") +
              darker(f("%03d", millis % 1000)) + " ";
#endif  // not defined(__EMSCRIPTEN__)
    for (int i = 0; i < indent; ++i) {
      buffer += " ";
    }
  }
};

Logger::Logger(LogLevel log_level, const std::source_location location)
    : impl(new Logger::Impl(log_level, location)) {}

Logger::~Logger() {
  if (impl->log_level == LOG_LEVEL_DISCARD) {
    delete impl;
    return;
  }
  std::string output = impl->buffer;

  if (impl->log_level == LOG_LEVEL_ERROR || impl->log_level == LOG_LEVEL_FATAL) {
    output += f(" (%s in %s:%d)", impl->location.function_name(), impl->location.file_name(),
                impl->location.line());
  }

#if defined(__EMSCRIPTEN__)
  if (impl->log_level == LOG_LEVEL_ERROR) {
    EM_ASM({ console.warn(UTF8ToString($0)); }, output.c_str());
  } else if (impl->log_level == LOG_LEVEL_FATAL) {
    EM_ASM({ console.error(UTF8ToString($0)); }, output.c_str());
  } else {
    EM_ASM({ console.log(UTF8ToString($0)); }, output.c_str());
  }
#else
  printf("%s\n", output.c_str());
#endif

  if (impl->log_level == LOG_LEVEL_FATAL) {
    abort();
  }
  delete impl;
}

const Logger& operator<<(const Logger& logger, int i) {
  logger.impl->buffer += std::to_string(i);
  return logger;
}

const Logger& operator<<(const Logger& logger, unsigned i) {
  logger.impl->buffer += std::to_string(i);
  return logger;
}

const Logger& operator<<(const Logger& logger, unsigned long i) {
  logger.impl->buffer += std::to_string(i);
  return logger;
}

const Logger& operator<<(const Logger& logger, unsigned long long i) {
  logger.impl->buffer += std::to_string(i);
  return logger;
}

const Logger& operator<<(const Logger& logger, float f) {
  logger.impl->buffer += std::to_string(f);
  return logger;
}

const Logger& operator<<(const Logger& logger, double d) {
  logger.impl->buffer += std::to_string(d);
  return logger;
}

const Logger& operator<<(const Logger& logger, std::string_view s) {
  logger.impl->buffer += s;
  return logger;
}

const Logger& operator<<(const Logger& logger, const unsigned char* s) {
  logger.impl->buffer += (const char*)s;
  return logger;
}

const Logger& operator<<(const Logger& logger, Vec2 v) {
  logger.impl->buffer += "Vec2( ";
  logger.impl->buffer += std::to_string(v.x);
  logger.impl->buffer += ", ";
  logger.impl->buffer += std::to_string(v.y);
  logger.impl->buffer += " )";
  return logger;
}

const Logger& operator<<(const Logger& logger, Vec3 v) {
  logger.impl->buffer += "Vec3( ";
  logger.impl->buffer += std::to_string(v.x);
  logger.impl->buffer += ", ";
  logger.impl->buffer += std::to_string(v.y);
  logger.impl->buffer += ", ";
  logger.impl->buffer += std::to_string(v.z);
  logger.impl->buffer += " )";
  return logger;
}
