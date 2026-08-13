#include "CDNAudioFile.h"
#include "BellUtils.h"

#include <string.h>          // for memcpy
#include <chrono>            // for steady_clock (chunk-fetch timing)
#include <functional>        // for __base
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <string_view>       // for string_view
#include <type_traits>       // for remove_extent_t

#include "AccessKeyFetcher.h"  // for AccessKeyFetcher
#include "BellLogger.h"        // for AbstractLogger
#include "Crypto.h"
#include "Logger.h"            // for CSPOT_LOG
#include "Packet.h"            // for cspot
#include "SocketStream.h"      // for SocketStream
#include "Utils.h"             // for bigNumAdd, bytesToHexString, string...
#include "WrappedSemaphore.h"  // for WrappedSemaphore
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace cspot;

CDNAudioFile::CDNAudioFile(const std::string& cdnUrl,
                           const std::vector<uint8_t>& audioKey)
    : cdnUrl(cdnUrl), audioKey(audioKey) {
  this->crypto = std::make_unique<Crypto>();
}

size_t CDNAudioFile::getPosition() {
  return this->position;
}

void CDNAudioFile::seek(size_t newPos) {
  this->enableRequestMargin = true;
  this->position = newPos;
}

bool CDNAudioFile::openStream() {
  CSPOT_LOG(info, "Opening HTTP stream to %s", this->cdnUrl.c_str());

  // Open connection, read first 128 bytes
  for (int cdnAttempt = 0; cdnAttempt < 5; cdnAttempt++) {
    this->httpConnection = bell::HTTPClient::get(
      this->cdnUrl,
      {bell::HTTPClient::RangeHeader::range(0, OPUS_HEADER_SIZE - 1)});
    if (this->httpConnection != nullptr) {
      break;
    }
    CSPOT_LOG(error, "CDN connection failed (attempt %d)", cdnAttempt + 1);
    BELL_SLEEP_MS(2000);
  }
  if (this->httpConnection == nullptr) {
    CSPOT_LOG(error, "CDN connection failed permanently");
    return false;  // exceptions-free build: caller skips the track
  }

  this->httpConnection->stream().read((char*)header.data(), OPUS_HEADER_SIZE);
  this->totalFileSize =
      this->httpConnection->totalLength() - SPOTIFY_OPUS_HEADER;

  this->decrypt(header.data(), OPUS_HEADER_SIZE, 0);

  // Location must be dividable by 16
  size_t footerStartLocation =
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) -
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) % 16;

  this->footer = std::vector<uint8_t>(
      this->totalFileSize - footerStartLocation + SPOTIFY_OPUS_HEADER);
  this->httpConnection->get(
      cdnUrl, {bell::HTTPClient::RangeHeader::last(footer.size())});

  this->httpConnection->stream().read((char*)footer.data(),
                                      this->footer.size());

  this->decrypt(footer.data(), footer.size(), footerStartLocation);
  CSPOT_LOG(info, "Header and footer bytes received");
  this->position = 0;
  this->lastRequestPosition = 0;
  this->lastRequestCapacity = 0;
  return true;
}

size_t CDNAudioFile::readBytes(uint8_t* dst, size_t bytes) {
  size_t offsetPosition = position + SPOTIFY_OPUS_HEADER;
  size_t actualFileSize = this->totalFileSize + SPOTIFY_OPUS_HEADER;

  if (position + bytes >= this->totalFileSize) {
    return 0;
  }

  // // Opus tries to read header, use prefetched data
  if (offsetPosition < OPUS_HEADER_SIZE &&
      bytes + offsetPosition <= OPUS_HEADER_SIZE) {
    memcpy(dst, this->header.data() + offsetPosition, bytes);
    position += bytes;
    return bytes;
  }

  // // Opus tries to read footer, use prefetched data
  if (offsetPosition >= (actualFileSize - this->footer.size())) {
    size_t toReadBytes = bytes;

    if ((position + bytes) > this->totalFileSize) {
      // Tries to read outside of bounds, truncate
      toReadBytes = this->totalFileSize - position;
    }

    size_t footerOffset =
        offsetPosition - (actualFileSize - this->footer.size());
    memcpy(dst, this->footer.data() + footerOffset, toReadBytes);

    position += toReadBytes;
    return toReadBytes;
  }

  // Data not in the headers. Make sense of whats going on.
  // Position in bounds :)
  if (offsetPosition >= this->lastRequestPosition &&
      offsetPosition < this->lastRequestPosition + this->lastRequestCapacity) {
    size_t toRead = bytes;

    if ((toRead + offsetPosition) >
        this->lastRequestPosition + lastRequestCapacity) {
      toRead = this->lastRequestPosition + lastRequestCapacity - offsetPosition;
    }

    memcpy(dst, this->httpBuffer.data() + offsetPosition - lastRequestPosition,
           toRead);
    position += toRead;

    return toRead;
  } else {
    size_t requestPosition = (offsetPosition) - ((offsetPosition) % 16);
    if (this->enableRequestMargin && requestPosition > SEEK_MARGIN_SIZE) {
      requestPosition = (offsetPosition - SEEK_MARGIN_SIZE) -
                        ((offsetPosition - SEEK_MARGIN_SIZE) % 16);
      this->enableRequestMargin = false;
    }

    // This refetch sits synchronously on the decode path (one HTTP round-trip
    // per HTTP_BUFFER_SIZE of audio, i.e. every ~0.5s of playback), so a slow
    // response directly drains the playback buffer. Time it to correlate
    // audible lags with CDN/Wi-Fi latency spikes.
    auto fetchStart = std::chrono::steady_clock::now();
    this->httpConnection->get(
        cdnUrl, {bell::HTTPClient::RangeHeader::range(
                    requestPosition, requestPosition + HTTP_BUFFER_SIZE - 1)});
    this->lastRequestPosition = requestPosition;
    this->lastRequestCapacity = this->httpConnection->contentLength();

    this->httpConnection->stream().read((char*)this->httpBuffer.data(),
                                        lastRequestCapacity);
    auto fetchMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - fetchStart)
                       .count();
    if (fetchMs > 300) {
      CSPOT_LOG(info, "CDN chunk fetch slow: %d ms @ pos=%u len=%u",
                (int)fetchMs, (unsigned)requestPosition,
                (unsigned)lastRequestCapacity);
    }
    this->decrypt(this->httpBuffer.data(), lastRequestCapacity,

                  this->lastRequestPosition);

    return readBytes(dst, bytes);
  }

  return bytes;
}

size_t CDNAudioFile::getSize() {
  return this->totalFileSize;
}

void CDNAudioFile::decrypt(uint8_t* dst, size_t nbytes, size_t pos) {
  auto calculatedIV = bigNumAdd(audioAESIV, pos / 16);

  this->crypto->aesCTRXcrypt(this->audioKey, calculatedIV, dst, nbytes);
}
