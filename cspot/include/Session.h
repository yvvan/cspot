#pragma once

#include <stdint.h>  // for uint8_t
#include <memory>    // for shared_ptr, unique_ptr
#include <mutex>     // for mutex
#include <string>    // for string
#include <vector>    // for vector

namespace cspot {
class AuthChallenges;
class LoginBlob;
class PlainConnection;
class ShannonConnection;
}  // namespace cspot

#define LOGIN_REQUEST_COMMAND 0xAB
#define AUTH_SUCCESSFUL_COMMAND 0xAC
#define AUTH_DECLINED_COMMAND 0xAD

namespace cspot {
class Session {
 protected:
  std::unique_ptr<cspot::AuthChallenges> challenges;
  std::shared_ptr<cspot::PlainConnection> conn;
  std::shared_ptr<LoginBlob> authBlob;
  std::mutex shanConnMutex;

  std::string deviceId = "142137fd329622137a14901634264e6f332e2411";

 public:
  Session();
  ~Session();

  std::shared_ptr<cspot::ShannonConnection> shanConn;

  /**
  * @brief Thread-safe snapshot of the current Shannon connection.
  *
  * Reconnection replaces shanConn from the Mercury task while other tasks
  * (Spirc notify, audio-key requests) may be sending — they must work on a
  * snapshot instead of the mutable member.
  */
  std::shared_ptr<cspot::ShannonConnection> shanConnection() {
    std::lock_guard<std::mutex> lock(shanConnMutex);
    return shanConn;
  }

  bool connect(std::unique_ptr<cspot::PlainConnection> connection);
  bool connectWithRandomAp();
  void close();
  virtual bool triggerTimeout() = 0;
  std::vector<uint8_t> authenticate(std::shared_ptr<LoginBlob> blob);
};
}  // namespace cspot
