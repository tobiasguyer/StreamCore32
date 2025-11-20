#include "SocketStream.h"

#include <stdint.h>  // for uint8_t
#include <cstdio>    // for NULL, ssize_t

#include "TCPSocket.h"  // for TCPSocket
#include "TLSSocket.h"  // for TLSSocket

using namespace bell;
ssize_t SocketStream::readSome(char* dst, size_t len) {
  auto* sb = rdbuf();
  if (!sb) throw std::runtime_error("No streambuf");
  return sb->readSome(dst, len);
}
ssize_t SocketStream::writeSome(const char* src, size_t len) {
  auto* sb = rdbuf();
  if (!sb) throw std::runtime_error("No streambuf");
  return sb->writeSome(src, len);
}
size_t SocketStream::available() {
  auto* sb = rdbuf();
  if (!sb) throw std::runtime_error("No streambuf");
  return sb->available();
}
int SocketBuffer::open(const std::string& hostname, int port, bool isSSL) {
  if (internalSocket != nullptr) {
    close();
  }
  if (isSSL) {
    internalSocket = std::make_unique<bell::TLSSocket>();
  } else {
    internalSocket = std::make_unique<bell::TCPSocket>();
  }

  internalSocket->open(hostname, port);
  return 0;
}

int SocketBuffer::close() {
  if (internalSocket != nullptr && isOpen()) {
    pubsync();
    internalSocket->close();
    internalSocket = nullptr;
  }
  return 0;
}
ssize_t SocketBuffer::readSome(char* dst, size_t len) {
  if (!internalSocket) throw std::runtime_error("Internal socket is null");
  if (!dst && len) throw std::invalid_argument("Destination buffer is null");
  return internalSocket->read(reinterpret_cast<uint8_t*>(dst), len);
}

ssize_t SocketBuffer::writeSome(const char* src, size_t len) {
  if (!internalSocket) throw std::runtime_error("Internal socket is null");
  if (!src && len) throw std::invalid_argument("Source buffer is null");
  return internalSocket->write(reinterpret_cast<const uint8_t*>(src), len);
}

size_t SocketBuffer::available() {
  if (!internalSocket) throw std::runtime_error("Internal socket is null");
  return internalSocket->poll();
}
std::streamsize SocketBuffer::showmanyc() {
  // bytes already buffered in get area
  const std::streamsize bn = egptr() - gptr();
  if (bn > 0) return bn;

  // If TLS/socket reports extra bytes available, tell iostream about it.
  if (internalSocket) {
    return static_cast<std::streamsize>(internalSocket->poll()); // TLS: mbedtls_ssl_get_bytes_avail
  }
  return 0;
}
int SocketBuffer::sync() {
  if (!internalSocket) {
    throw std::runtime_error("Internal socket is null");
  }
  ssize_t bw, n = pptr() - pbase();
  while (n > 0) {
    bw = internalSocket->write(reinterpret_cast<uint8_t*>(pptr() - n), n);
    if (bw < 0) {
      setp(pptr() - n, obuf + bufLen);
      pbump(n);
      return -1;
    }
    n -= bw;
  }
  setp(obuf, obuf + bufLen);
  return 0;
}

SocketBuffer::int_type SocketBuffer::underflow() {
  if (!internalSocket) {
    throw std::runtime_error("Internal socket is null");
  }
  ssize_t br = internalSocket->read(reinterpret_cast<uint8_t*>(ibuf), bufLen);
  if (br <= 0) {
    setg(NULL, NULL, NULL);
    return traits_type::eof();
  }
  setg(ibuf, ibuf, ibuf + br);
  return traits_type::to_int_type(*ibuf);
}

SocketBuffer::int_type SocketBuffer::overflow(int_type c) {
  if (sync() < 0)
    return traits_type::eof();
  if (traits_type::eq_int_type(c, traits_type::eof()))
    return traits_type::not_eof(c);
  *pptr() = traits_type::to_char_type(c);
  pbump(1);
  return c;
}

std::streamsize SocketBuffer::xsgetn(char_type* __s, std::streamsize __n) {
  // Validate the internal socket
  if (!internalSocket) {
    throw std::runtime_error("Internal socket is null");
  }

  // Validate the destination buffer
  if (!__s) {
    throw std::invalid_argument("Destination buffer is null");
  }

  const std::streamsize bn = egptr() - gptr();
  if (__n <= bn) {
    traits_type::copy(__s, gptr(), __n);
    gbump(__n);
    return __n;
  }

  // Copy the available data first, then refill
  traits_type::copy(__s, gptr(), bn);
  setg(NULL, NULL, NULL);
  std::streamsize remain = __n - bn;
  char_type* end = __s + __n;

  ssize_t br;
  while (remain > 0) {
    br = internalSocket->read(reinterpret_cast<uint8_t*>(end - remain), remain);
    if (br <= 0)
      return (__n - remain);
    remain -= br;
  }

  return __n;
}
std::streamsize SocketBuffer::xsputn(const char_type* __s,
                                     std::streamsize __n) {
  if (pptr() + __n <= epptr()) {
    traits_type::copy(pptr(), __s, __n);
    pbump(__n);
    return __n;
  }
  if (sync() < 0)
    return 0;
  ssize_t bw;
  std::streamsize remain = __n;
  const char_type* end = __s + __n;
  while (remain > bufLen) {
    bw = internalSocket->write((uint8_t*)(end - remain), remain);
    if (bw < 0)
      return (__n - remain);
    remain -= bw;
  }
  if (remain > 0) {
    traits_type::copy(pptr(), end - remain, remain);
    pbump(remain);
  }
  return __n;
}
