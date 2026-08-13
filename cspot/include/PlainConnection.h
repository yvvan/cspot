#ifndef PLAINCONNECTION_H
#define PLAINCONNECTION_H
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include "win32shim.h"
#else
#include <unistd.h>  // for size_t
#endif
#include <cstdint>     // for uint8_t
#include <functional>  // for function
#include <string>      // for string
#include <vector>      // for vector

typedef std::function<bool()> timeoutCallback;

namespace cspot {
class PlainConnection {
 public:
  PlainConnection();
  ~PlainConnection();

  /**
   * @brief Connect to the given AP address
   * 
   * @param apAddress The AP url to connect to
   */
  bool connect(const std::string& apAddress);
  void close();

  timeoutCallback timeoutHandler;
  std::vector<uint8_t> sendPrefixPacket(const std::vector<uint8_t>& prefix,
                                        const std::vector<uint8_t>& data);
  std::vector<uint8_t> recvPacket();

  void readBlock(const uint8_t* dst, size_t size);
  size_t writeBlock(const std::vector<uint8_t>& data);

  // Set when a socket read/write hits an unrecoverable error. The exceptions-free
  // build can't throw to unwind, so the Mercury loop polls this and reconnects
  // instead of aborting the whole device.
  bool disconnected = false;
  bool isDisconnected() const { return disconnected; }

 private:
  // Plain (pre-Shannon) packets only carry the AP handshake; anything larger
  // than this is a corrupted size read off a dying connection.
  static constexpr uint32_t MAX_PLAIN_PACKET_SIZE = 512 * 1024;

  int apSock;
};
}  // namespace cspot

#endif