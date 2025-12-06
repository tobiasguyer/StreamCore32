#include "Session.h"

#include <limits.h>     // for CHAR_BIT
#include <cstdint>      // for uint8_t
#include <functional>   // for __base
#include <memory>       // for shared_ptr, unique_ptr, make_unique
#include <random>       // for default_random_engine, independent_bi...
#include <type_traits>  // for remove_extent_t
#include <utility>      // for move
#include <cstring>      // for memcpy

#include "ApResolve.h"          // for ApResolve, spotify
#include "AuthChallenges.h"     // for AuthChallenges
#include "BellLogger.h"         // for AbstractLogger
#include "Logger.h"             // for SC32_LOG
#include "LoginBlob.h"          // for LoginBlob
#include "Packet.h"             // for Packet
#include "PlainConnection.h"    // for PlainConnection, timeoutCallback
#include "ShannonConnection.h"  // for ShannonConnection

#include "NanoPBHelper.h"  // for pbPutString, pbEncode, pbDecode
#include "pb_decode.h"
#include "protobuf/authentication.pb.h"

using namespace spotify;

Session::Session() {
  this->challenges = std::make_unique<spotify::AuthChallenges>();
}

Session::~Session() {}

void Session::connect(std::unique_ptr<spotify::PlainConnection> connection) {
  this->conn = std::move(connection);
  conn->timeoutHandler = [this]() {
    return this->triggerTimeout();
    };
  auto helloPacket = this->conn->sendPrefixPacket(
    { 0x00, 0x04 }, this->challenges->prepareClientHello());
  auto apResponse = this->conn->recvPacket();
  auto solvedHello = this->challenges->solveApHello(helloPacket, apResponse);
  conn->sendPrefixPacket({}, solvedHello);

  // Generates the public and priv key
  this->shanConn = std::make_shared<ShannonConnection>();

  // Init shanno-encrypted connection
  this->shanConn->wrapConnection(this->conn, challenges->shanSendKey,
    challenges->shanRecvKey);
}

void Session::connectWithRandomAp() {
  auto apResolver = std::make_unique<ApResolve>("");
  auto conn = std::make_unique<spotify::PlainConnection>();
  conn->timeoutHandler = [this]() {
    return this->triggerTimeout();
    };

  auto apAddr = apResolver->fetchFirstApAddress();

  conn->connect(apAddr);

  this->connect(std::move(conn));
}

std::vector<uint8_t> Session::authenticate(std::shared_ptr<LoginBlob> blob) {
  // save auth blob for reconnection purposes
  authBlob = blob;
  // prepare authentication request proto
  auto data = challenges->prepareAuthPacket(
    blob->authData, blob->authType, blob->getDeviceId(), blob->username);
  // Send login request
  this->shanConn->sendPacket(LOGIN_REQUEST_COMMAND, data);
  auto packet = this->shanConn->recvPacket();
  switch (packet.command) {
  case AUTH_SUCCESSFUL_COMMAND: {
    APWelcome welcome = {};
    welcome = pbDecode<APWelcome>(APWelcome_fields, packet.data);
    std::vector<uint8_t> key(
      welcome.reusable_auth_credentials.bytes,
      welcome.reusable_auth_credentials.bytes
      + welcome.reusable_auth_credentials.size
    );
    pb_release(APWelcome_fields, &welcome);
    return key;
    break;
  }
  case AUTH_DECLINED_COMMAND: {
    SC32_LOG(error, "Authorization declined");
    break;
  }
  default:
    SC32_LOG(error, "Unknown auth fail code %d", packet.command);
  }

  return std::vector<uint8_t>(0);
}

void Session::close() {
  this->conn->close();
}
