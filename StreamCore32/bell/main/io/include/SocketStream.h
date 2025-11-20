#pragma once

#include <iostream>  // for streamsize, basic_streambuf<>::int_type, ios...
#include <memory>    // for unique_ptr, operator!=
#include <string>    // for char_traits, string

#include "BellSocket.h"  // for Socket
#include "BellUtils.h"

namespace bell {
class SocketBuffer : public std::streambuf {
 private:
  std::unique_ptr<bell::Socket> internalSocket;

  static const int bufLen = 1024;
  char ibuf[bufLen], obuf[bufLen];

 public:
  SocketBuffer() { internalSocket = nullptr; }

  SocketBuffer(const std::string& hostname, int port, bool isSSL = false) {
    open(hostname, port);
  }

  int open(const std::string& hostname, int port, bool isSSL = false);

  int close();

  bool isOpen() {
    return internalSocket != nullptr && internalSocket->isOpen();
  }
  ssize_t readSome(char* dst, size_t len);
  ssize_t writeSome(const char* src, size_t len);
  size_t  available();
  ~SocketBuffer() { close(); }
  virtual std::streamsize showmanyc() override;

 protected:
  virtual int sync();

  virtual int_type underflow();

  virtual int_type overflow(int_type c = traits_type::eof());

  virtual std::streamsize xsgetn(char_type* __s, std::streamsize __n);

  virtual std::streamsize xsputn(const char_type* __s, std::streamsize __n);
};

class SocketStream : public std::iostream {
 private:
  SocketBuffer socketBuf;

 public:
  SocketStream() : std::iostream(&socketBuf) {}

  SocketStream(const std::string& hostname, int port, bool isSSL = false)
      : std::iostream(&socketBuf) {
    open(hostname, port, isSSL);
  }

  SocketBuffer* rdbuf() { return &socketBuf; }

  int open(const std::string& hostname, int port, bool isSSL = false) {
    int err = socketBuf.open(hostname, port, isSSL);
    if (err)
      setstate(std::ios::failbit);
    return err;
  }

  int close() { return socketBuf.close(); }
ssize_t readSome(char* dst, size_t len);
    ssize_t writeSome(const char* src, size_t len);
    size_t  available() ;
    size_t readExact(char* dst, size_t n, uint32_t idle_timeout_ms = 5000) {
  size_t total = 0;
  uint32_t idle = 0;
  while (total < n) {
    this->read(dst + total, n - total);
    std::streamsize got = this->gcount();
    if (got <= 0) {
      if (!isOpen()) break;          // bubble up closed socket
      BELL_SLEEP_MS(5);
      idle += 5;
      if (idle >= idle_timeout_ms) break;
      continue;
    }
    idle = 0;
    total += size_t(got);
  }
  return total; // == n on success
}
  bool isOpen() { return socketBuf.isOpen(); }
};
}  // namespace bell
