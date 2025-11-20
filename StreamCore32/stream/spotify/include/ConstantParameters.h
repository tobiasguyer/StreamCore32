#pragma once

#define MAX_VOLUME 65536

// variable weakly set in ZeroconfAuthentificator.cpp
extern char deviceId[];

namespace spotify {
// Hardcoded information sent to spotify servers
const char* const informationString = "StreamCore32-player";
const char* const brandName = "StreamCore32";
const char* const versionString = "StreamCore32-1.1";
const char* const protocolVersion = "2.7.1";
const char* const defaultDeviceName = "StreamCore32";
const char* const swVersion = "1.0.0";

}  // namespace spotify