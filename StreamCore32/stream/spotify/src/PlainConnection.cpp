#include "PlainConnection.h"

#ifndef _WIN32
#include <netdb.h>  // for addrinfo, freeaddrinfo, getaddrinfo
#include <netdb.h>
#include <netinet/in.h>   // for IPPROTO_IP, IPPROTO_TCP
#include <netinet/tcp.h>  // for TCP_NODELAY
#include <sys/errno.h>    // for EAGAIN, EINTR, ETIMEDOUT, errno
#include <sys/socket.h>   // for setsockopt, connect, recv, send, shutdown
#include <sys/time.h>     // for timeval
#include <cstring>        // for memset
#include <stdexcept>      // for runtime_error
#else
#include <ws2tcpip.h>
#endif
#include "BellLogger.h"  // for AbstractLogger
#include "BellUtils.h"   // for BELL_SLEEP
#include "Logger.h"      // for SC32_LOG
#include "Packet.h"      // for spotify
#include "Utils.h"       // for extract, pack

using namespace spotify;

static int getErrno() {
#ifdef _WIN32
  int code = WSAGetLastError();
  if (code == WSAETIMEDOUT)
    return ETIMEDOUT;
  if (code == WSAEINTR)
    return EINTR;
  return code;
#else
  return errno;
#endif
}

PlainConnection::PlainConnection() {
  this->apSock = -1;
};

PlainConnection::~PlainConnection() {
  this->close();
};

void PlainConnection::connect(const std::string& apAddress) {
  struct addrinfo h, *airoot, *ai;
  std::string hostname = apAddress.substr(0, apAddress.find(":"));
  std::string portStr =
      apAddress.substr(apAddress.find(":") + 1, apAddress.size());
  memset(&h, 0, sizeof(h));
  h.ai_family = AF_INET;
  h.ai_socktype = SOCK_STREAM;
  h.ai_protocol = IPPROTO_IP;

  // Lookup host
  if (getaddrinfo(hostname.c_str(), portStr.c_str(), &h, &airoot)) {
    SC32_LOG(error, "getaddrinfo failed");
  }

  // find the right ai, connect to server
  for (ai = airoot; ai; ai = ai->ai_next) {
    if (ai->ai_family != AF_INET && ai->ai_family != AF_INET6)
      continue;

    this->apSock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (this->apSock < 0)
      continue;

    if (::connect(this->apSock, (struct sockaddr*)ai->ai_addr,
                  ai->ai_addrlen) != -1) {
#ifdef _WIN32
      uint32_t tv = 3000;
#else
      struct timeval tv;
      tv.tv_sec = 3;
      tv.tv_usec = 0;
#endif
      setsockopt(this->apSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv,
                 sizeof tv);
      setsockopt(this->apSock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv,
                 sizeof tv);

      int flag = 1;
      setsockopt(this->apSock, /* socket affected */
                 IPPROTO_TCP,  /* set option at TCP level */
                 TCP_NODELAY,  /* name of option */
                 (char*)&flag, /* the cast is historical cruft */
                 sizeof(int)); /* length of option value */
      break;
    }

#ifdef _WIN32
    closesocket(this->apSock);
#else
    ::close(this->apSock);
#endif
    apSock = -1;
    throw std::runtime_error("Can't connect to spotify servers");
  }

  freeaddrinfo(airoot);
  SC32_LOG(debug, "Connected to spotify server");
}

std::vector<uint8_t> PlainConnection::recvPacket() {
  // Read packet size
  std::vector<uint8_t> packetBuffer(4);
  readBlock(packetBuffer.data(), 4);
  uint32_t packetSize = ntohl(extract<uint32_t>(packetBuffer, 0));

  packetBuffer.resize(packetSize, 0);

  // Read actual data
  readBlock(packetBuffer.data() + 4, packetSize - 4);

  return packetBuffer;
}

std::vector<uint8_t> PlainConnection::sendPrefixPacket(
    const std::vector<uint8_t>& prefix, const std::vector<uint8_t>& data) {
  // Calculate full packet length
  uint32_t actualSize = prefix.size() + data.size() + sizeof(uint32_t);

  // Packet structure [PREFIX] + [SIZE] +  [DATA]
  auto sizeRaw = pack<uint32_t>(htonl(actualSize));
  sizeRaw.insert(sizeRaw.begin(), prefix.begin(), prefix.end());
  sizeRaw.insert(sizeRaw.end(), data.begin(), data.end());

  // Actually write it to the server
  writeBlock(sizeRaw);

  return sizeRaw;
}
size_t PlainConnection::writeBlock(const std::vector<uint8_t>& data) {
  size_t idx = 0;

  while (idx < data.size()) {
    const size_t toSend = data.size() - idx;  // let the OS split as needed
#ifdef MSG_NOSIGNAL
    ssize_t n = ::send(apSock, reinterpret_cast<const char*>(&data[idx]),
                       toSend, MSG_NOSIGNAL);
#else
    ssize_t n =
        ::send(apSock, reinterpret_cast<const char*>(&data[idx]), toSend, 0);
#endif
    if (n > 0) {
      idx += static_cast<size_t>(n);
      continue;
    }

    if (n == 0) {
      SC32_LOG(error, "write: send returned 0 (peer?)");
      throw std::runtime_error("Peer closed");
    }

    const int e = getErrno();
    if (e == EAGAIN
#ifdef EWOULDBLOCK
        || e == EWOULDBLOCK
#endif
        || e == ETIMEDOUT) {
      if (timeoutHandler()) {
        SC32_LOG(error, "write: timeoutHandler() says reconnect");
        throw std::runtime_error("Reconnection required");
      }
      continue;
    }
    if (e == EINTR)
      continue;
    if (e == EPIPE || e == ECONNRESET) {
      SC32_LOG(error, "write: connection lost (errno=%d %s)", e, strerror(e));
      throw std::runtime_error("Reconnection required");
    }

    SC32_LOG(error, "write: fatal errno=%d (%s)", e, strerror(e));
    throw std::runtime_error("Error in write");
  }
  return data.size();
}

void PlainConnection::readBlock(uint8_t* dst, size_t size) {
  size_t idx = 0;

  while (idx < size) {
    ssize_t n =
        ::recv(apSock, reinterpret_cast<char*>(dst + idx), size - idx, 0);
    if (n > 0) {
      idx += static_cast<size_t>(n);
      continue;
    }

    if (n == 0) {
      SC32_LOG(error, "read: peer closed (recv==0)");
      throw std::runtime_error("Peer closed");
    }

    const int e = getErrno();
    if (e == EAGAIN
#ifdef EWOULDBLOCK
        || e == EWOULDBLOCK
#endif
        || e == ETIMEDOUT) {
      // Soft timeout: ask the higher-level timeoutHandler
      if (timeoutHandler()) {
        SC32_LOG(error, "read: timeoutHandler() says reconnect");
        throw std::runtime_error("Reconnection required");
      }
      continue;  // try again
    }

    if (e == EINTR)
      continue;

    SC32_LOG(error, "read: fatal errno=%d (%s)", e, strerror(e));
    throw std::runtime_error("Error in read");
  }
}

void PlainConnection::close() {
  if (this->apSock < 0)
    return;

  SC32_LOG(info, "Closing socket...");
  shutdown(this->apSock, SHUT_RDWR);
#ifdef _WIN32
  closesocket(this->apSock);
#else
  ::close(this->apSock);
#endif
  this->apSock = -1;
}
