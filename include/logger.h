#pragma once

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

// Format a 32-bit siteID as an 8-digit uppercase hex string (e.g. "8C70CE74").
inline std::string siteToHex(uint32_t id) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08X", id);
  return std::string(buf);
}

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

// Thread-safe singleton logger that writes structured lines to a file.
// Call Logger::init() once at startup; all subsequent log calls from any
// thread are safe.  If init() is never called (e.g. in tests), every log
// call is a silent no-op.
class Logger {
public:
  // Open log file in append mode and store siteID.  No-op if path is empty.
  static void init(const std::string &path, uint32_t siteID,
                   LogLevel minLevel = LogLevel::INFO);

  // Flush and close the log file.
  static void shutdown();

  // Write one log line.  No-op if uninitialized or level < minLevel.
  // Format: [YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [SITEHHEX] [module] msg
  static void log(LogLevel level, const std::string &module,
                  const std::string &msg);

  static void debug(const std::string &m, const std::string &s);
  static void info(const std::string &m, const std::string &s);
  static void warn(const std::string &m, const std::string &s);
  static void error(const std::string &m, const std::string &s);

private:
  static std::mutex mutex_;
  static std::ofstream file_;
  static uint32_t siteID_;
  static LogLevel minLevel_;
  static bool initialized_;
};

// Compile-time DEBUG suppression.
// Build with -DENABLE_DEBUG_LOG (cmake option ENABLE_DEBUG_LOG=ON) to include
// DEBUG entries; otherwise they expand to a zero-cost no-op.
#ifdef ENABLE_DEBUG_LOG
#define LOG_DEBUG(module, msg) Logger::debug(module, msg)
#else
#define LOG_DEBUG(module, msg)                                                 \
  do {                                                                         \
  } while (false)
#endif

#define LOG_INFO(module, msg) Logger::info(module, msg)
#define LOG_WARN(module, msg) Logger::warn(module, msg)
#define LOG_ERROR(module, msg) Logger::error(module, msg)
