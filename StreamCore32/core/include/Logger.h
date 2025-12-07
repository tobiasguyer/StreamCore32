#pragma once

#include <BellLogger.h>
#include <stdarg.h>  // va_list, va_start, va_end
#include <stdio.h>   // vsnprintf
#include <functional>
#include <string>
extern std::function<bool(const std::string&)> WsSendJsonSCLogger;
inline void SC32_SendToWs(std::string filename, int line, const char* fmt,
                          ...) {
  if (!WsSendJsonSCLogger) {
    return;
  }

  va_list args;
  va_start(args, fmt);

  // First, try with a fixed small buffer
  char stackBuf[256];
  int len = vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);

  va_end(args);

  if (len < 0) {
    return;  // formatting error
  }

  std::string msg;

  if (len < (int)sizeof(stackBuf)) {
    // Fits in stackBuf
    msg.assign(stackBuf, len);
  } else {
    // Need a bigger buffer: call vsnprintf again
    std::string buf(len + 1, '\0');

    va_list args2;
    va_start(args2, fmt);
    vsnprintf(buf.data(), buf.size(), fmt, args2);
    va_end(args2);

    msg.assign(buf.c_str(), len);
  }
#ifdef _WIN32
  std::string basenameStr(filename.substr(filename.rfind("\\") + 1));
#else
  std::string basenameStr(filename.substr(filename.rfind("/") + 1));
#endif
  std::string full =
      std::string(basenameStr) + ":" + std::to_string(line) + " " + msg;

  WsSendJsonSCLogger(full);
}
#define SC32_LOG(type, fmt, ...)                                        \
  do {                                                                  \
    bell::bellGlobalLogger->type(__FILE__, __LINE__, "streamcore", fmt, \
                                 ##__VA_ARGS__);                        \
    SC32_SendToWs(__FILE__, __LINE__, fmt, ##__VA_ARGS__);              \
  } while (0)
