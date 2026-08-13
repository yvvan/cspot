#include "Session.h"

#include <limits.h>     // for CHAR_BIT
#include <cstdint>      // for uint8_t
#include <functional>   // for __base
#include <memory>       // for shared_ptr, unique_ptr, make_unique
#include <random>       // for default_random_engine, independent_bi...
#include <type_traits>  // for remove_extent_t
#include <utility>      // for move

#include "ApResolve.h"          // for ApResolve, cspot
#include "AuthChallenges.h"     // for AuthChallenges
#include "BellLogger.h"         // for AbstractLogger
#include "Logger.h"             // for CSPOT_LOG
#include "LoginBlob.h"          // for LoginBlob
#include "Packet.h"             // for Packet
#include "PlainConnection.h"    // for PlainConnection, timeoutCallback
#include "ShannonConnection.h"  // for ShannonConnection

#include "NanoPBHelper.h"  // for pbPutString, pbEncode, pbDecode
#include "pb_decode.h"
#include "protobuf/authentication.pb.h"

using random_bytes_engine =
    std::independent_bits_engine<std::default_random_engine, CHAR_BIT, uint8_t>;

using namespace cspot;

Session::Session() {
  this->challenges = std::make_unique<cspot::AuthChallenges>();
}

Session::~Session() {}

bool Session::connect(std::unique_ptr<cspot::PlainConnection> connection) {
  this->conn = std::move(connection);
  conn->timeoutHandler = [this]() {
    return this->triggerTimeout();
  };
  auto helloPacket = this->conn->sendPrefixPacket(
      {0x00, 0x04}, this->challenges->prepareClientHello());
  auto apResponse = this->conn->recvPacket();
  // The AP resetting the connection mid-handshake leaves apResponse empty or
  // truncated; solveApHello would walk past its end (std::length_error, which
  // aborts in the exceptions-free build). Fail the connect instead — the
  // caller's retry loop handles it.
  if (this->conn->isDisconnected() || apResponse.size() <= 4) {
    CSPOT_LOG(error, "AP hello handshake failed (connection lost, %u bytes)",
              (unsigned)apResponse.size());
    return false;
  }
  CSPOT_LOG(info, "Received APHello response");

  auto solvedHello = this->challenges->solveApHello(helloPacket, apResponse);

  conn->sendPrefixPacket({}, solvedHello);
  if (this->conn->isDisconnected()) {
    CSPOT_LOG(error, "AP handshake failed while sending client response");
    return false;
  }
  CSPOT_LOG(debug, "Received shannon keys");

  // Build the shannon-encrypted connection fully, then publish it atomically —
  // senders on other tasks snapshot it via shanConnection().
  auto newShanConn = std::make_shared<ShannonConnection>();
  newShanConn->wrapConnection(this->conn, challenges->shanSendKey,
                              challenges->shanRecvKey);
  {
    std::lock_guard<std::mutex> lock(shanConnMutex);
    this->shanConn = newShanConn;
  }
  return true;
}

bool Session::connectWithRandomAp() {
  auto apResolver = std::make_unique<ApResolve>("");
  auto conn = std::make_unique<cspot::PlainConnection>();
  conn->timeoutHandler = [this]() {
    return this->triggerTimeout();
  };

  auto apAddr = apResolver->fetchFirstApAddress();
  if (apAddr.empty()) {
    CSPOT_LOG(error, "AP resolve failed; will retry later");
    return false;
  }

  CSPOT_LOG(debug, "Connecting with AP <%s>", apAddr.c_str());
  if (!conn->connect(apAddr)) {
    return false;
  }

  return this->connect(std::move(conn));
}

std::vector<uint8_t> Session::authenticate(std::shared_ptr<LoginBlob> blob) {
  // save auth blob for reconnection purposes
  authBlob = blob;
  // prepare authentication request proto
  auto data = challenges->prepareAuthPacket(blob->authData, blob->authType,
                                            deviceId, blob->username);

  // Send login request
  this->shanConn->sendPacket(LOGIN_REQUEST_COMMAND, data);

  auto packet = this->shanConn->recvPacket();
  if (this->shanConn->isDisconnected()) {
    CSPOT_LOG(error, "Connection lost while waiting for auth response");
    return std::vector<uint8_t>(0);
  }
  switch (packet.command) {
    case AUTH_SUCCESSFUL_COMMAND: {
      APWelcome welcome;
      CSPOT_LOG(debug, "Authorization successful");
      pbDecode(welcome, APWelcome_fields, packet.data);
      return std::vector<uint8_t>(welcome.reusable_auth_credentials.bytes,
                                  welcome.reusable_auth_credentials.bytes +
                                      welcome.reusable_auth_credentials.size);
      break;
    }
    case AUTH_DECLINED_COMMAND: {
      CSPOT_LOG(error, "Authorization declined");
      break;
    }
    default:
      CSPOT_LOG(error, "Unknown auth fail code %d", packet.command);
  }

  return std::vector<uint8_t>(0);
}

void Session::close() {
  this->conn->close();
}
