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
#include "BellTask.h"          // for Task (prefetch reader)
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

// The prefetch reader runs below the player task's priority (network work must
// not preempt audio) with a PSRAM stack sized for a TLS reconnect handshake.
class CDNAudioFile::ReaderTask : public bell::Task {
 public:
  explicit ReaderTask(CDNAudioFile* owner)
      : bell::Task("cdn_prefetch", 16 * 1024, 2, 1), owner(owner) {
    startTask();
  }

 protected:
  void runTask() override { owner->prefetchLoop(); }

 private:
  CDNAudioFile* owner;
};

CDNAudioFile::CDNAudioFile(const std::string& cdnUrl,
                           const std::vector<uint8_t>& audioKey)
    : cdnUrl(cdnUrl), audioKey(audioKey) {
  this->crypto = std::make_unique<Crypto>();
}

CDNAudioFile::~CDNAudioFile() {
  stopReader();
}

void CDNAudioFile::stopReader() {
  if (readerTask == nullptr)
    return;
  {
    std::lock_guard<std::mutex> lock(ringMutex);
    readerShouldStop = true;
  }
  ringCv.notify_all();
  readerExited->wait();
  // The task epilogue (vTaskDelete) still runs briefly on the PSRAM stack the
  // ReaderTask destructor frees — give it a moment.
  BELL_SLEEP_MS(50);
  readerTask.reset();
}

size_t CDNAudioFile::getPosition() {
  return this->position;
}

void CDNAudioFile::seek(size_t newPos) {
  std::lock_guard<std::mutex> lock(ringMutex);
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

  // Start the prefetch reader over the body region (header and footer above
  // are served from their own buffers).
  stopReader();
  {
    std::lock_guard<std::mutex> lock(ringMutex);
    ring.clear();
    nextFetchPosition = OPUS_HEADER_SIZE;
    fetchEndPosition = this->totalFileSize + SPOTIFY_OPUS_HEADER - footer.size();
    fetchGeneration++;
    readerShouldStop = false;
    streamFailed = false;
  }
  readerExited = std::make_unique<bell::WrappedSemaphore>(1);
  readerTask = std::make_unique<ReaderTask>(this);
  return true;
}

void CDNAudioFile::prefetchLoop() {
  int consecutiveFailures = 0;

  while (true) {
    size_t fetchPos = 0;
    uint32_t generation = 0;
    {
      std::unique_lock<std::mutex> lock(ringMutex);
      ringCv.wait(lock, [&] {
        return readerShouldStop || (ring.size() < PREFETCH_CHUNK_COUNT &&
                                    nextFetchPosition < fetchEndPosition);
      });
      if (readerShouldStop)
        break;
      fetchPos = nextFetchPosition;
      generation = fetchGeneration;
    }

    PrefetchChunk chunk;
    bool ok = fetchChunk(fetchPos, chunk);  // network + decrypt, no lock held

    bool fatal = false;
    {
      std::lock_guard<std::mutex> lock(ringMutex);
      if (readerShouldStop)
        break;
      if (generation != fetchGeneration)
        continue;  // a seek repointed the reader while this fetch was in flight
      if (ok) {
        consecutiveFailures = 0;
        nextFetchPosition = chunk.position + chunk.data.size();
        ring.push_back(std::move(chunk));
        ringCv.notify_all();
        continue;
      }
      if (++consecutiveFailures >= 3) {
        CSPOT_LOG(error, "CDN prefetch failed repeatedly; failing the stream");
        streamFailed = true;
        ringCv.notify_all();
        fatal = true;
      }
    }
    if (fatal)
      break;
    BELL_SLEEP_MS(1000);
  }
  readerExited->give();
}

bool CDNAudioFile::fetchChunk(size_t requestPosition, PrefetchChunk& out) {
  size_t requestEnd = requestPosition + HTTP_BUFFER_SIZE;
  if (requestEnd > fetchEndPosition)
    requestEnd = fetchEndPosition;
  if (requestEnd <= requestPosition)
    return false;

  auto fetchStart = std::chrono::steady_clock::now();

  auto rangeHeader =
      bell::HTTPClient::RangeHeader::range(requestPosition, requestEnd - 1);
  size_t len = 0;
  if (httpConnection != nullptr) {
    httpConnection->get(cdnUrl, {rangeHeader});
    len = httpConnection->contentLength();
  }
  if (len == 0 || len > (size_t)HTTP_BUFFER_SIZE) {
    // Keep-alive connection went stale — replace it.
    CSPOT_LOG(info, "CDN connection stale (len=%u); reconnecting",
              (unsigned)len);
    httpConnection = bell::HTTPClient::get(cdnUrl, {rangeHeader});
    if (httpConnection == nullptr)
      return false;
    len = httpConnection->contentLength();
    if (len == 0 || len > (size_t)HTTP_BUFFER_SIZE)
      return false;
  }

  out.data.resize(len);
  auto& stream = httpConnection->stream();
  stream.read((char*)out.data.data(), len);
  if ((size_t)stream.gcount() != len) {
    CSPOT_LOG(error, "CDN read short (%d of %u bytes)", (int)stream.gcount(),
              (unsigned)len);
    httpConnection = nullptr;  // force a fresh connection on retry
    return false;
  }

  this->decrypt(out.data.data(), len, requestPosition);
  out.position = requestPosition;

  auto fetchMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - fetchStart)
                     .count();
  if (fetchMs > 1000) {
    CSPOT_LOG(info, "CDN chunk fetch slow: %d ms @ pos=%u len=%u", (int)fetchMs,
              (unsigned)requestPosition, (unsigned)len);
  }
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

  // Body data — served from the prefetch ring. The reader task keeps the ring
  // topped up; the only time this blocks is when the network is slower than
  // playback for longer than the whole ring covers.
  std::unique_lock<std::mutex> lock(ringMutex);
  while (true) {
    if (streamFailed || readerShouldStop)
      return 0;

    if (!ring.empty()) {
      auto& chunk = ring.front();
      if (offsetPosition >= chunk.position &&
          offsetPosition < chunk.position + chunk.data.size()) {
        size_t chunkOffset = offsetPosition - chunk.position;
        size_t toRead = bytes;
        if (toRead > chunk.data.size() - chunkOffset)
          toRead = chunk.data.size() - chunkOffset;

        memcpy(dst, chunk.data.data() + chunkOffset, toRead);
        position += toRead;

        if (chunkOffset + toRead == chunk.data.size()) {
          ring.pop_front();
          ringCv.notify_all();
        }
        return toRead;
      }
      // The decoder moved (seek) — repoint the reader and refill.
      repositionLocked(offsetPosition);
      continue;
    }

    repositionLocked(offsetPosition);
    ringCv.wait_for(lock, std::chrono::milliseconds(100));
  }
}

void CDNAudioFile::repositionLocked(size_t offsetPosition) {
  // Already on course: the next fetch will produce a chunk containing the
  // requested offset.
  if (ring.empty() && nextFetchPosition <= offsetPosition &&
      offsetPosition < nextFetchPosition + HTTP_BUFFER_SIZE)
    return;
  // Or the offset sits in a later chunk already in the ring — drop the front.
  if (!ring.empty() && offsetPosition >= ring.front().position) {
    ring.pop_front();
    ringCv.notify_all();
    return;
  }

  size_t requestPosition = offsetPosition - (offsetPosition % 16);
  if (this->enableRequestMargin && requestPosition > (size_t)SEEK_MARGIN_SIZE) {
    // vorbis routinely steps a little backwards right after a coarse seek;
    // starting the window early avoids an immediate second reposition.
    requestPosition = (offsetPosition - SEEK_MARGIN_SIZE) -
                      ((offsetPosition - SEEK_MARGIN_SIZE) % 16);
    this->enableRequestMargin = false;
  }

  ring.clear();
  fetchGeneration++;
  nextFetchPosition = requestPosition;
  ringCv.notify_all();
}

size_t CDNAudioFile::getSize() {
  return this->totalFileSize;
}

void CDNAudioFile::decrypt(uint8_t* dst, size_t nbytes, size_t pos) {
  auto calculatedIV = bigNumAdd(audioAESIV, pos / 16);

  this->crypto->aesCTRXcrypt(this->audioKey, calculatedIV, dst, nbytes);
}
