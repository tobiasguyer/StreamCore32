#pragma once
#ifdef _WIN32
    #include <basetsd.h>
    typedef SSIZE_T ssize_t;
#endif
#include <string>
#include <sys/types.h>

namespace bell {
class Socket {
 public:
  Socket(){};
  virtual ~Socket() = default;

  virtual int open(const std::string& host, uint16_t port) = 0;
  virtual size_t poll() = 0;
  virtual ssize_t write(const uint8_t* buf, size_t len) = 0;
  virtual ssize_t read(uint8_t* buf, size_t len) = 0;
  virtual bool isOpen() = 0;
  virtual void close() = 0;
  virtual int getFd() = 0;
};
}  // namespace bell
