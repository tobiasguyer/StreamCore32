#pragma once

#include <cstdint>
#include <vector>

namespace spotify {
struct Packet {
  uint8_t command;
  std::vector<uint8_t> data;
};
}  // namespace spotify