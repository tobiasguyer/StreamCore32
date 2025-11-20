#pragma once

#include <stdint.h>  // for uint8_t
#include <memory>    // for shared_ptr, unique_ptr
#include <string>    // for string
#include <vector>    // for vector

namespace spotify {
class AuthChallenges;
class LoginBlob;
class PlainConnection;
class ShannonConnection;
}  // namespace spotify

#define LOGIN_REQUEST_COMMAND 0xAB
#define AUTH_SUCCESSFUL_COMMAND 0xAC
#define AUTH_DECLINED_COMMAND 0xAD

namespace spotify {
class Session {
 protected:
  std::unique_ptr<spotify::AuthChallenges> challenges;
  std::shared_ptr<spotify::PlainConnection> conn;
  std::shared_ptr<LoginBlob> authBlob;

  //std::string deviceId = "142137fd329622137a14901634264e6f332e2411";

 public:
  Session();
  ~Session();

  std::shared_ptr<spotify::ShannonConnection> shanConn;

  void connect(std::unique_ptr<spotify::PlainConnection> connection);
  void connectWithRandomAp();
  void close();
  virtual bool triggerTimeout() = 0;
  std::vector<uint8_t> authenticate(std::shared_ptr<LoginBlob> blob);
};
}  // namespace spotify
