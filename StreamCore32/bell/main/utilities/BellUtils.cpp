#include "BellUtils.h"
#include <stdio.h>
#include <stdlib.h>

#include <random>    // for mt19937, uniform_int_distribution, random_device
#ifdef ESP_PLATFORM
#include "esp_system.h"
#if __has_include("esp_mac.h")
#include "esp_mac.h"
#endif
#elif defined(_WIN32) || defined(_WIN64)
#include <iphlpapi.h>
#pragma comment(lib, "IPHLPAPI.lib")
#else
#include <ifaddrs.h>
#include <net/if.h>
#if defined(__APPLE__)
#include <net/if_dl.h>
#else
#include <netpacket/packet.h>
#endif
#endif

std::string bell::generateRandomUUID() {
  static std::random_device dev;
  static std::mt19937 rng(dev());

  std::uniform_int_distribution<int> dist(0, 15);

  const char* v = "0123456789abcdef";
  const bool dash[] = {0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0};

  std::string res;
  for (int i = 0; i < 16; i++) {
    if (dash[i])
      res += "-";
    res += v[dist(rng)];
    res += v[dist(rng)];
  }
  return res;
}

std::string bell::getMacAddress() {
#if defined(ESP_PLATFORM)

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char macStr[18];
  sprintf(macStr, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2],
          mac[3], mac[4], mac[5]);
  return std::string(macStr);
#elif defined(_WIN32) || defined(_WIN64)
  // Return the MAC of the first UP adapter with a non-zero physical address.
  ULONG bufLen = 16 * 1024;
  IP_ADAPTER_ADDRESSES* addrs = static_cast<IP_ADAPTER_ADDRESSES*>(malloc(bufLen));
  if (!addrs) return "00:00:00:00:00:00";

  ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_PHYSICAL_ADDRESS;
  DWORD ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &bufLen);
  if (ret == ERROR_BUFFER_OVERFLOW) {
    auto* bigger = static_cast<IP_ADAPTER_ADDRESSES*>(realloc(addrs, bufLen));
    if (!bigger) { free(addrs); return "00:00:00:00:00:00"; }
    addrs = bigger;
    ret = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, addrs, &bufLen);
  }
  std::string macStr = "00:00:00:00:00:00";
  if (ret == NO_ERROR) {
    for (auto* a = addrs; a; a = a->Next) {
      if (a->OperStatus != IfOperStatusUp) continue;
      if (a->PhysicalAddressLength >= 6) {
        char buf[18];
        sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x",
                a->PhysicalAddress[0], a->PhysicalAddress[1],
                a->PhysicalAddress[2], a->PhysicalAddress[3],
                a->PhysicalAddress[4], a->PhysicalAddress[5]);
        macStr.assign(buf);
        break;
      }
    }
  }
  free(addrs);
  return macStr;
#else
  // POSIX: Use getifaddrs; prefer the first UP, non-loopback interface with a MAC.
  struct ifaddrs* ifs = nullptr;
  if (getifaddrs(&ifs) != 0 || !ifs) {
    return std::string("00:00:00:00:00:00");
  }
  std::string macStr = "00:00:00:00:00:00";
  for (auto* ifa = ifs; ifa; ifa = ifa->ifa_next) {
    if (!ifa->ifa_addr || !(ifa->ifa_flags & IFF_UP) || (ifa->ifa_flags & IFF_LOOPBACK))
      continue;
#if defined(__APPLE__)
    if (ifa->ifa_addr->sa_family == AF_LINK) {
      auto* sdl = reinterpret_cast<const struct sockaddr_dl*>(ifa->ifa_addr);
      const unsigned char* mac = (const unsigned char*)LLADDR(sdl);
      if (sdl->sdl_alen >= 6) {
        char buf[18];
        sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        macStr.assign(buf);
        break;
      }
    }
#else
    if (ifa->ifa_addr->sa_family == AF_PACKET) {
      auto* s = reinterpret_cast<const struct sockaddr_ll*>(ifa->ifa_addr);
      if (s->sll_halen >= 6) {
        char buf[18];
        sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x",
                (unsigned char)s->sll_addr[0], (unsigned char)s->sll_addr[1],
                (unsigned char)s->sll_addr[2], (unsigned char)s->sll_addr[3],
                (unsigned char)s->sll_addr[4], (unsigned char)s->sll_addr[5]);
        macStr.assign(buf);
        break;
      }
    }
#endif
  }
  freeifaddrs(ifs);
  return macStr;
#endif
  return "00:00:00:00:00:00";
}

void bell::freeAndNull(void*& ptr) {
  free(ptr);
  ptr = nullptr;
}
