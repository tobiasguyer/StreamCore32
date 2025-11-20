#include "TLSSocket.h"

#include <mbedtls/ctr_drbg.h>     // for mbedtls_ctr_drbg_free, mbedtls_ctr_...
#include <mbedtls/entropy.h>      // for mbedtls_entropy_free, mbedtls_entro...
#include <mbedtls/error.h>        // for mbedtls_ssl_conf_authmode, mbedtls_...
#include <mbedtls/net_sockets.h>  // for mbedtls_net_connect, mbedtls_net_free
#include <mbedtls/ssl.h>          // for mbedtls_ssl_conf_authmode, mbedtls_...
#include <lwip/sockets.h>         // for close
#include <cstring>                // for strlen, NULL
#include <stdexcept>              // for runtime_error

#include "BellLogger.h"  // for AbstractLogger, BELL_LOG
#include "BellUtils.h"   // for BELL_SLEEP_MS
#include "X509Bundle.h"  // for shouldVerify, attach

/**
 * Platform TLSSocket implementation for the mbedtls
 */
bell::TLSSocket::TLSSocket() : isClosed(false) {
  mbedtls_net_init(&server_fd);
  mbedtls_ssl_init(&ssl);
  mbedtls_ssl_config_init(&conf);

  if (bell::X509Bundle::shouldVerify()) {
    bell::X509Bundle::attach(&conf);
  }

  mbedtls_ctr_drbg_init(&ctr_drbg);
  mbedtls_entropy_init(&entropy);

  const char* pers = "euphonium";
  int ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                  reinterpret_cast<const unsigned char*>(pers),
                                  strlen(pers));
  if (ret != 0) {
    BELL_LOG(error, "http_tls", "Failed to seed DRBG: %d\n", ret);
    throw std::runtime_error("Failed to seed DRBG");
  }
}

// TLSSocket.cpp
int bell::TLSSocket::open(const std::string& host, uint16_t port) {
  int ret = mbedtls_net_connect(&server_fd, host.c_str(),
                                std::to_string(port).c_str(), MBEDTLS_NET_PROTO_TCP);
  if (ret != 0) { BELL_LOG(error,"http_tls","net_connect %d", ret); return ret; }

  ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) { BELL_LOG(error,"http_tls","config %d", ret); return ret; }


  if (bell::X509Bundle::shouldVerify()) {
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  } else {
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
  }
  mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
  //  mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
  mbedtls_ssl_setup(&ssl, &conf);

  if ((ret = mbedtls_ssl_set_hostname(&ssl, host.c_str())) != 0) {
    throw std::runtime_error("Failed to set SSL hostname");
  }

  mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv,
                      NULL);
  for (int tries=0; tries<2; ++tries) {
    int retries = 5;
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
      if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) { BELL_SLEEP_MS(10); continue; }
      BELL_LOG(error,"http_tls","handshake %d", ret);
      if (--retries > 0) { BELL_SLEEP_MS(50); continue; }
      // fatal for this TCP socket: break to reconnect once
      break;
    }
    if (ret == 0) { isClosed = false; return 0; }
    // Reconnect TCP and try handshake again once
    close();
    ret = mbedtls_net_connect(&server_fd, host.c_str(),
                              std::to_string(port).c_str(), MBEDTLS_NET_PROTO_TCP);
    if (ret != 0) { BELL_LOG(error,"http_tls","reconnect %d", ret); return ret; }
    mbedtls_ssl_session_reset(&ssl);
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);
  }
  return ret ? ret : 0;
}


ssize_t bell::TLSSocket::read(uint8_t* buf, size_t len) {
  int ret = mbedtls_ssl_read(&ssl, buf, len);

  if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) { close(); return 0; }
  if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    return 0;                  // <- was -EAGAIN: treat as "no data yet"
  if (ret == 0) { close(); return 0; } // orderly close from peer
  if (ret < 0) { close(); return -1; } // real error
  return static_cast<ssize_t>(ret);
}

ssize_t bell::TLSSocket::write(const uint8_t* buf, size_t len) {
  int ret;
  do {
    ret = mbedtls_ssl_write(&ssl, buf, len);
  } while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE);

  if (ret < 0) {
    BELL_LOG(error, "http_tls", "Write error with code %x\n", ret);
    close();  //isClosed = true;
  }
  return static_cast<ssize_t>(ret);
}
int bell::TLSSocket::poll_readable(int timeout_ms) {    // Fast path: already-decrypted bytes waiting inside mbedTLS
    if (mbedtls_ssl_get_bytes_avail(&ssl) > 0) return 1;

    // Raw socket readiness via lwIP
    if (server_fd.fd < 0) return -1;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(server_fd.fd, &rfds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int rc = lwip_select(server_fd.fd + 1, &rfds, nullptr, nullptr,
                         (timeout_ms >= 0 ? &tv : nullptr));
    // rc > 0: readable, 0: timeout, <0: error
    return rc;
}
size_t bell::TLSSocket::poll() {
  return mbedtls_ssl_get_bytes_avail(&ssl);
}

bool bell::TLSSocket::isOpen() {
  return !isClosed;
}

void bell::TLSSocket::close() {
  if (isClosed)
    return;
  (void)mbedtls_ssl_close_notify(&ssl);
  mbedtls_net_free(&server_fd);
  mbedtls_net_init(&server_fd);
  mbedtls_ssl_session_reset(&ssl);
  isClosed = true;
}
