// SPDX-FileCopyrightText: Copyright 2024 Automat Authors
// SPDX-License-Identifier: MIT
#include "log.hh"

#if defined(__linux__)
#include <unistd.h>
#elif defined(_WIN32)
#include <io.h>
#endif

#include "format.hh"
#include "path.hh"

namespace automat {

std::vector<Logger>& GetLoggers() {
  static std::vector<Logger> loggers = [] {
    std::vector<Logger> loggers;
    loggers.emplace_back(DefaultLogger);
    return loggers;
  }();
  return loggers;
}

static int indent = 0;

void LOG_Indent(int n) { indent += n; }

void LOG_Unindent(int n) { indent -= n; }

LogEntry::LogEntry(LogLevel log_level, const std::source_location location)
    : log_level(log_level),
      timestamp(std::chrono::system_clock::now()),
      location(location),
      buffer(),
      errsv(errno) {
  for (int i = 0; i < indent; ++i) {
    buffer += " ";
  }
}

StrView CleanFunctionName(const char* func) {
  StrView name(func);
  // Remove args
  auto paren_pos = name.find('(');
  if (paren_pos != StrView::npos) {
    name = name.substr(0, paren_pos);
  }
  // Remove return type
  auto last_space_pos = name.rfind(' ');
  if (last_space_pos != StrView::npos) {
    name = name.substr(last_space_pos + 1);
  }
  // Remove namespaces
  while (true) {
    if (name.size() > 0 && isupper(name[0])) {
      break;
    }
    auto colon_pos = name.find("::");
    if (colon_pos == StrView::npos) {
      break;
    }
    name = name.substr(colon_pos + 2);
  }
  return name;
}

LogEntry::~LogEntry() {
  if (log_level == LogLevel::Ignore) {
    return;
  }

  if (log_level == LogLevel::Fatal) {
    buffer += f(" Crashing in {}:{} [{}].", location.file_name(), location.line(),
                location.function_name());
  }

  if (log_level == LogLevel::Error) {
    buffer += f(" ({} at {}:{})", CleanFunctionName(location.function_name()),
                Path(location.file_name()).Name(), location.line());
  }

  auto& loggers = GetLoggers();
  for (auto& logger : loggers) {
    logger(*this);
  }

  if (log_level == LogLevel::Fatal) {
    fflush(stdout);
    fflush(stderr);
    abort();
  }
}

void DefaultLogger(const LogEntry& e) {
#if defined(__EMSCRIPTEN__)
  if (e.log_level == LogLevel::Error) {
    EM_ASM({ console.warn(UTF8ToString($0)); }, e.buffer.c_str());
  } else if (e.log_level == LogLevel::Fatal) {
    EM_ASM({ console.error(UTF8ToString($0)); }, e.buffer.c_str());
  } else {
    EM_ASM({ console.log(UTF8ToString($0)); }, e.buffer.c_str());
  }
#elif defined(__linux__)
  auto line = e.buffer + '\n';
  (void)write(STDOUT_FILENO, line.c_str(), line.size());
#elif defined(_WIN32)
  auto line = e.buffer + '\n';
  _write(1, line.c_str(), (unsigned int)line.size());
#else
  printf("%s\n", e.buffer.c_str());
#endif
}

const LogEntry& operator<<(const LogEntry& logger, StrView s) {
  logger.buffer += s;
  return logger;
}

const LogEntry& operator<<(const LogEntry& logger, Status& status) {
  logger.buffer += status.ToStr();
  logger.errsv = status.errsv;
  return logger;
}

}  // namespace automat
