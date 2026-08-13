#pragma once

#include <atomic>      // or std::atomic
#include <functional>  // for function
#include <memory>      // for shared_ptr
#include <string>      // for string

namespace cspot {
struct Context;

class AccessKeyFetcher {
 public:
  AccessKeyFetcher(std::shared_ptr<cspot::Context> ctx);

  /**
  * @brief Checks if key is expired
  * @returns true when currently held access key is not valid
  */
  bool isExpired();

  /**
  * @brief Fetches a new access key
  * @remark In case the key is expired, this function blocks until a refresh is done.
  * @returns access key
  */
  std::string getAccessKey();

  /**
  * @brief Forces a refresh of the access key
  * @remark A failed fetch backs off exponentially: the caller polls this on
  *         every queue tick, and without a backoff a server-side rejection
  *         turns into hundreds of keymaster requests per minute.
  */
  void updateAccessKey();

 private:
  static constexpr long long int RETRY_BASE_MS = 1000;
  static constexpr long long int RETRY_MAX_MS = 60000;

  void onFetchFailed(const char* reason);

  std::shared_ptr<cspot::Context> ctx;

  std::atomic<bool> keyPending = false;
  std::string accessKey;
  long long int expiresAt = 0;
  long long int nextAttemptAt = 0;
  int consecutiveFailures = 0;
};
}  // namespace cspot
