#include "logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>

// Static member definitions.
std::mutex Logger::mutex_{};
std::ofstream Logger::file_{};
uint32_t Logger::siteID_ = 0;
LogLevel Logger::minLevel_ = LogLevel::INFO;
bool Logger::initialized_ = false;

void Logger::init(const std::string &path, uint32_t siteID, LogLevel minLevel) {
  if (path.empty())
    return;
  std::lock_guard<std::mutex> lk(mutex_);
  file_.open(path, std::ios::app);
  siteID_ = siteID;
  minLevel_ = minLevel;
  initialized_ = file_.is_open();
}

void Logger::shutdown() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!initialized_)
    return;
  file_.flush();
  file_.close();
  initialized_ = false;
}

void Logger::log(LogLevel level, const std::string &module,
                 const std::string &msg) {
  if (!initialized_ || level < minLevel_)
    return;

  // Capture timestamp before acquiring the lock to avoid measuring contention.
  auto now = std::chrono::system_clock::now();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) %
            1000;
  std::time_t tt = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif

  static const char *levelStr[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
  int lvlIdx = static_cast<int>(level);
  if (lvlIdx < 0 || lvlIdx > 3)
    lvlIdx = 3;

  char timeBuf[32];
  std::snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));

  char siteBuf[16];
  std::snprintf(siteBuf, sizeof(siteBuf), "%08X", siteID_);

  std::lock_guard<std::mutex> lk(mutex_);
  file_ << '[' << timeBuf << "] [" << levelStr[lvlIdx] << "] [" << siteBuf
        << "] [" << module << "] " << msg << '\n';
  file_.flush();
}

void Logger::debug(const std::string &m, const std::string &s) {
  log(LogLevel::DEBUG, m, s);
}
void Logger::info(const std::string &m, const std::string &s) {
  log(LogLevel::INFO, m, s);
}
void Logger::warn(const std::string &m, const std::string &s) {
  log(LogLevel::WARN, m, s);
}
void Logger::error(const std::string &m, const std::string &s) {
  log(LogLevel::ERROR, m, s);
}
