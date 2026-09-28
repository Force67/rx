#include "base/atomic.h"
#include "base/strings/string_ref.h"
#include "base/threading/lock_guard.h"
#include "base/threading/mutex.h"
#include "foundation/logging/log.h"

#include "foundation/system/app_identity.h"

#include <stdio.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace rx {
namespace {

base::Atomic<LogLevel> g_level{LogLevel::kInfo};
base::Mutex g_mutex;

const char* LevelTag(LogLevel level) {
  switch (level) {
    case LogLevel::kTrace: return "trace";
    case LogLevel::kDebug: return "debug";
    case LogLevel::kInfo: return "info";
    case LogLevel::kWarn: return "warn";
    case LogLevel::kError: return "error";
  }
  return "?";
}

}  // namespace

void SetLogLevel(LogLevel level) { g_level.store(level); }

namespace detail {

void LogMessage(LogLevel level, base::StringRef message) {
  if (level < g_level.load()) return;
#if defined(__ANDROID__)
  __android_log_print(ANDROID_LOG_INFO, GetAppIdentity().name.c_str(), "[%s] %.*s", LevelTag(level),
                      static_cast<int>(message.size()), message.data());
#else
  base::LockGuard lock(g_mutex);
  FILE* out = level >= LogLevel::kWarn ? stderr : stdout;
  ::fprintf(out, "[%s] %.*s\n", LevelTag(level), static_cast<int>(message.size()),
            message.data());
  // Redirected to a file (a launcher with no console) stdout is fully
  // buffered: a crash would lose the lines leading up to it.
  ::fflush(out);
#endif
}

}  // namespace detail
}  // namespace rx
