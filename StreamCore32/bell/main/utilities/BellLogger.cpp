#include "BellLogger.h"

// Single global lock for logging across the whole program
bell::WrappedMutex logMutex;

bell::AbstractLogger* bell::bellGlobalLogger;

void bell::setDefaultLogger() {
  bell::bellGlobalLogger = new bell::BellLogger();
}

void bell::enableSubmoduleLogging() {
  bell::bellGlobalLogger->enableSubmodule = true;
  bell::LockGuard _g(::logMutex);
  if (!bell::bellGlobalLogger) {
    bell::setDefaultLogger();
  }
  bell::bellGlobalLogger->enableSubmodule = true;
}

void bell::enableTimestampLogging(bool local) {
  if (!bell::bellGlobalLogger) {
    bell::setDefaultLogger();
  }
  bell::LockGuard _g(::logMutex);
  if (!bell::bellGlobalLogger) {
    bell::setDefaultLogger();
  }
  bell::bellGlobalLogger->enableTimestamp = true;
  bell::bellGlobalLogger->shortTime = local;
}
