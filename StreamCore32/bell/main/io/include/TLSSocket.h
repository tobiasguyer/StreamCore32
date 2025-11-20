#ifndef BELL_TLS_SOCKET_H
#define BELL_TLS_SOCKET_H

#include <stdint.h>  // for uint8_t, uint16_t

#include "BellSocket.h"  // for Socket
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#endif
#include <stdlib.h>  // for size_t
#include <string>    // for string

#include "mbedtls/ctr_drbg.h"     // for mbedtls_ctr_drbg_context
#include "mbedtls/entropy.h"      // for mbedtls_entropy_context
#include "mbedtls/net_sockets.h"  // for mbedtls_net_context
#include "mbedtls/ssl.h"          // for mbedtls_ssl_config, mbedtls_ssl_con...

namespace bell {
class TLSSocket : public bell::Socket {
 private:
  mbedtls_net_context server_fd;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context ctr_drbg;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;

  bool isClosed = true;

 public:
  TLSSocket();
  inline ~TLSSocket(){
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
  }
  int open(const std::string& host, uint16_t port);
  void wrapFd(int fd){};  // @TODO: implement

  ssize_t read(uint8_t* buf, size_t len);
  ssize_t write(const uint8_t* buf, size_t len);
  size_t poll();
  int poll_readable(int timeout_ms = 0);

  bool isOpen();

  void close();
  int getFd() { return server_fd.fd; }
};
}  // namespace bell

#endif