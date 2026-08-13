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
#include "NanoPBExtensions.h"  // for bell::nanopb::encodeString/encodeVector
#include "NanoPBHelper.h"      // for pbEncode / pbDecode
#include "MercurySession.h"    // for MercurySession, MercurySession::Res...
#include "TimeProvider.h"
#include "Utils.h"

#include "protobuf/login5.pb.h"  // for LoginRequest / LoginResponse

#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif


using namespace cspot;

// Spotify's web client id, the one login5 accepts for stored-credential logins.
static std::string CLIENT_ID = "65b708073fc0480ea92a077233ca87bd";

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

  // login5, not the Mercury keymaster: Spotify is retiring keymaster, and by 2026-08 it answers
  // every token request with {"code":2,"errorDescription":"Invalid client"} — playback then dies
  // at CDN-URL resolution with a perfectly healthy session. This is the upstream cspot path
  // (also where librespot went), restored on top of this fork's backoff.
  LoginRequest loginRequest = LoginRequest_init_zero;
  LoginResponse loginResponse = LoginResponse_init_zero;

  loginRequest.client_info.client_id.funcs.encode = &bell::nanopb::encodeString;
  loginRequest.client_info.client_id.arg = &CLIENT_ID;
  loginRequest.client_info.device_id.funcs.encode = &bell::nanopb::encodeString;
  loginRequest.client_info.device_id.arg = &ctx->config.deviceId;

  loginRequest.which_login_method = LoginRequest_stored_credential_tag;
  loginRequest.login_method.stored_credential.username.funcs.encode =
      &bell::nanopb::encodeString;
  loginRequest.login_method.stored_credential.username.arg = &ctx->config.username;
  loginRequest.login_method.stored_credential.data.funcs.encode =
      &bell::nanopb::encodeVector;
  loginRequest.login_method.stored_credential.data.arg = &ctx->config.authData;

  auto encodedRequest = pbEncode(LoginRequest_fields, &loginRequest);
  CSPOT_LOG(info, "Fetching access token via login5 (%u bytes)",
            (unsigned)encodedRequest.size());

  auto response = bell::HTTPClient::post(
      "https://login5.spotify.com/v3/login",
      {{"Content-Type", "application/x-protobuf"}}, encodedRequest);
  if (response == nullptr) {
    onFetchFailed("login5 request failed");
    keyPending = false;
    return;
  }

  auto responseBytes = response->bytes();
  pbDecode(loginResponse, LoginResponse_fields, responseBytes);

  if (loginResponse.which_response == LoginResponse_ok_tag) {
    accessKey = std::string(loginResponse.response.ok.access_token);
    // Refresh at half the advertised lifetime, so a turn never races the expiry.
    int expiresIn = loginResponse.response.ok.has_access_token_expires_in
                        ? loginResponse.response.ok.access_token_expires_in / 2
                        : 1800;
    expiresAt = ctx->timeProvider->getSyncedTimestamp() + (expiresIn * 1000);
    consecutiveFailures = 0;
    nextAttemptAt = 0;
    CSPOT_LOG(info, "Access token fetched successfully (expires in %d s)", expiresIn * 2);
  } else if (loginResponse.which_response == LoginResponse_error_tag) {
    CSPOT_LOG(error, "login5 error %d", (int)loginResponse.response.error);
    onFetchFailed("login5 refused the stored credential");
  } else {
    // Anything else (e.g. a proof-of-work challenge, which this build does not solve).
    onFetchFailed("login5 answered with neither a token nor an error");
  }

  pb_release(LoginResponse_fields, &loginResponse);
  keyPending = false;
}
