#include "AccessKeyFetcher.h"

#include <cstring>           // for strrchr
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <type_traits>       // for remove_extent_t
#include <vector>            // for vector

#include "BellLogger.h"    // for AbstractLogger
#include "CSpotContext.h"  // for Context
#include "HTTPClient.h"
#include "Logger.h"            // for CSPOT_LOG
#include "MercurySession.h"    // for MercurySession, MercurySession::Res...
#include "TimeProvider.h"
#include "Utils.h"

#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif


using namespace cspot;

// Client ID and secret are set via ctx->config.clientId / clientSecret
// (registered Spotify app — never hardcode here)

AccessKeyFetcher::AccessKeyFetcher(std::shared_ptr<cspot::Context> ctx)
    : ctx(ctx) {}

bool AccessKeyFetcher::isExpired() {
  if (accessKey.empty()) {
    return true;
  }

  if (ctx->timeProvider->getSyncedTimestamp() > expiresAt) {
    return true;
  }

  return false;
}

std::string AccessKeyFetcher::getAccessKey() {
  if (!isExpired()) {
    return accessKey;
  }

  updateAccessKey();

  return accessKey;
}

void AccessKeyFetcher::onFetchFailed(const char* reason) {
  consecutiveFailures += 1;
  long long int delay = RETRY_BASE_MS;
  for (int i = 1; i < consecutiveFailures && delay < RETRY_MAX_MS; i++) {
    delay *= 2;
  }
  if (delay > RETRY_MAX_MS) {
    delay = RETRY_MAX_MS;
  }
  nextAttemptAt = ctx->timeProvider->getSyncedTimestamp() + delay;
  CSPOT_LOG(error, "Access token fetch failed (%s), attempt %d; retrying in %d ms",
            reason, consecutiveFailures, (int)delay);
}

void AccessKeyFetcher::updateAccessKey() {
  if (keyPending) {
    return;
  }
  if (ctx->timeProvider->getSyncedTimestamp() < nextAttemptAt) {
    return;  // backing off after a failed fetch
  }
  keyPending = true;

  auto timeProvider = this->ctx->timeProvider;
  
  std::string url = string_format(
      "hm://keymaster/token/authenticated?client_id=9a8d2f0ce77a4e248bb71fefcb557637&device_id=%s&scope=streaming",
      this->ctx->config.deviceId.c_str());

  ctx->session->execute(
      cspot::MercurySession::RequestType::GET, url,
      [this, timeProvider](cspot::MercurySession::Response& res) {
        if (res.fail || res.parts.empty()) {
           this->onFetchFailed("mercury request failed");
           this->keyPending = false;
           return;
        }
        char* accessKeyJson = (char*)res.parts[0].data();
        auto accessJSON = std::string(accessKeyJson, strrchr(accessKeyJson, '}') - accessKeyJson + 1);
#ifdef BELL_ONLY_CJSON
        cJSON* jsonBody = cJSON_Parse(accessJSON.c_str());
        if (jsonBody) {
           cJSON* token = cJSON_GetObjectItem(jsonBody, "accessToken");
           cJSON* expires = cJSON_GetObjectItem(jsonBody, "expiresIn");
           if (token && token->valuestring) {
              this->accessKey = token->valuestring;
              int expiresIn = expires ? expires->valueint / 2 : 1800;
              this->expiresAt = timeProvider->getSyncedTimestamp() + (expiresIn * 1000);
              this->consecutiveFailures = 0;
              this->nextAttemptAt = 0;
              CSPOT_LOG(info, "Access token fetched successfully");
           } else {
              this->onFetchFailed("no accessToken in response");
           }
           cJSON_Delete(jsonBody);
        } else {
           this->onFetchFailed("unparseable response");
        }
#else
        auto jsonBody = nlohmann::json::parse(accessJSON, nullptr, false);
        if (!jsonBody.is_discarded() && jsonBody.contains("accessToken")) {
           this->accessKey = jsonBody["accessToken"];
           int expiresIn = jsonBody.value("expiresIn", 3600) / 2;
           this->expiresAt = timeProvider->getSyncedTimestamp() + (expiresIn * 1000);
           this->consecutiveFailures = 0;
           this->nextAttemptAt = 0;
           CSPOT_LOG(info, "Access token fetched successfully");
        } else {
           CSPOT_LOG(error, "Keymaster rejected the token request: %s", accessJSON.c_str());
           this->onFetchFailed("bad response");
        }
#endif
        this->keyPending = false;
      });
}
