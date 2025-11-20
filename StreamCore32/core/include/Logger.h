#pragma once

#include <BellLogger.h>

#define SPOTIFY_LOG(type, ...)                                                \
  do {                                                                      \
    bell::bellGlobalLogger->type(__FILE__, __LINE__, "spotify", __VA_ARGS__); \
  } while (0)
