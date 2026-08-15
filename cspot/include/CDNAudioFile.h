#pragma once

#include <atomic>              // for atomic
#include <condition_variable>  // for condition_variable
#include <cstddef>             // for size_t
#include <cstdint>             // for uint8_t
#include <deque>               // for deque
#include <memory>              // for shared_ptr, unique_ptr
#include <mutex>               // for mutex
#include <string>              // for string
#include <vector>              // for vector

#include "Crypto.h"      // for Crypto
#include "HTTPClient.h"  // for HTTPClient

namespace bell {
class WrappedSemaphore;
}  // namespace bell

namespace cspot {
class AccessKeyFetcher;

class CDNAudioFile {

 public:
  CDNAudioFile(const std::string& cdnUrl, const std::vector<uint8_t>& audioKey);
  ~CDNAudioFile();

  /**
  * @brief Opens connection to the provided cdn url, and fetches track metadata.
  */
  bool openStream();

  /**
  * @brief Read and decrypt part of the cdn stream
  *
  * @param dst buffer where to read received data to
  * @param amount of bytes to read
  *
  * @returns amount of bytes read
  */
  size_t readBytes(uint8_t* dst, size_t bytes);

  /**
  * @brief Returns current position in CDN stream
  */
  size_t getPosition();

  /**
  * @brief returns total size of the audio file in bytes
  */
  size_t getSize();

  /**
  * @brief Seeks the track to provided position
  * @param position position where to seek the track
  */
  void seek(size_t position);

 private:
  const int OPUS_HEADER_SIZE = 8 * 1024;
  const int OPUS_FOOTER_PREFFERED = 1024 * 12;  // 12K should be safe
  const int SEEK_MARGIN_SIZE = 1024 * 4;

  const int HTTP_BUFFER_SIZE = 1024 * 14;
  const int SPOTIFY_OPUS_HEADER = 167;

  // Used to store opus metadata, speeds up read
  std::vector<uint8_t> header = std::vector<uint8_t>(OPUS_HEADER_SIZE);
  std::vector<uint8_t> footer;

  // AES IV for decrypting the audio stream
  const std::vector<uint8_t> audioAESIV = {0x72, 0xe0, 0x67, 0xfb, 0xdd, 0xcb,
                                           0xcf, 0x77, 0xeb, 0xe8, 0xbc, 0x64,
                                           0x3f, 0x63, 0x0d, 0x93};
  std::unique_ptr<Crypto> crypto;

  // Owned by the reader task once it is running; only openStream (before the
  // task starts) and the task itself touch it.
  std::unique_ptr<bell::HTTPClient::Response> httpConnection;

  size_t position = 0;
  size_t totalFileSize = 0;

  bool enableRequestMargin = false;

  std::string cdnUrl;
  std::vector<uint8_t> audioKey;

  void decrypt(uint8_t* dst, size_t nbytes, size_t pos);

  // --- prefetch ring ---
  // A dedicated reader task keeps the next chunks downloaded and decrypted
  // ahead of the decoder, so a CDN/Wi-Fi latency spike drains the ring instead
  // of stalling the audio path.
  //
  // Depth is set by the worst stall to ride out, not by the average one. On a busy 2.4 GHz
  // channel single 14KB GETs have been measured taking 10s and once 22s, while the link
  // itself stayed healthy (RSSI -35 dB, the control WebSocket never dropped) — 8 chunks
  // covered ~6s of audio and those stalls went straight through as silence.
  static constexpr size_t PREFETCH_CHUNK_COUNT = 32;  // x 14KB ≈ 450KB (PSRAM) ≈ 22s of audio

  struct PrefetchChunk {
    size_t position = 0;  // absolute offset in the encrypted CDN file
    std::vector<uint8_t> data;
  };

  class ReaderTask;

  void prefetchLoop();
  bool fetchChunk(size_t requestPosition, PrefetchChunk& out);
  // Requires ringMutex held: points the reader at offsetPosition unless the
  // current fetch plan already covers it.
  void repositionLocked(size_t offsetPosition);
  void stopReader();

  std::unique_ptr<ReaderTask> readerTask;
  std::unique_ptr<bell::WrappedSemaphore> readerExited;

  std::mutex ringMutex;
  std::condition_variable ringCv;
  std::deque<PrefetchChunk> ring;
  size_t nextFetchPosition = 0;
  size_t fetchEndPosition = 0;  // first offset past the ring-served body
  uint32_t fetchGeneration = 0;
  bool readerShouldStop = false;
  bool streamFailed = false;
};
}  // namespace cspot
