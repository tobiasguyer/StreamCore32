#include "CDNAudioFile.h"

#include <string.h>          // for memcpy
#include <functional>        // for __base
#include <initializer_list>  // for initializer_list
#include <map>               // for operator!=, operator==
#include <string_view>       // for string_view
#include <type_traits>       // for remove_extent_t

#include "AccessKeyFetcher.h"  // for AccessKeyFetcher
#include "BellLogger.h"        // for AbstractLogger
#include "Crypto.h"
#include "Logger.h"            // for SC32_LOG
#include "Packet.h"            // for spotify
#include "SocketStream.h"      // for SocketStream
#include "Utils.h"             // for bigNumAdd, bytesToHexString, string...
#include "WrappedSemaphore.h"  // for WrappedSemaphore
#ifdef BELL_ONLY_CJSON
#include "cJSON.h"
#else
#include "nlohmann/json.hpp"      // for basic_json<>::object_t, basic_json
#include "nlohmann/json_fwd.hpp"  // for json
#endif

using namespace spotify;

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

#ifndef CONFIG_BELL_NOCODEC
/**
  * @brief Opens connection to the provided cdn url, and fetches track metadata.
  */
bool CDNAudioFile::openStream() {
  // Open connection, read first 128 bytes
  auto resp = bell::HTTPClient::get(
      this->cdnUrl,
      {bell::HTTPClient::RangeHeader::range(0, OPUS_HEADER_SIZE - 1)}, false);
  if (!resp->stream().isOpen() || resp->httpCode() != 200) {
    return false;
  }
  size_t got = resp->stream().readExact((char*)header.data(), OPUS_HEADER_SIZE);
  resp->stream().close();
  if (got != OPUS_HEADER_SIZE) {
    return false;
  }
  this->totalFileSize = resp->totalLength() - SPOTIFY_OPUS_HEADER;

  this->decrypt(header.data(), OPUS_HEADER_SIZE, 0);

  // Location must be dividable by 16
  size_t footerStartLocation =
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) -
      (this->totalFileSize - OPUS_FOOTER_PREFFERED + SPOTIFY_OPUS_HEADER) % 16;

  this->footer = std::vector<uint8_t>(
      this->totalFileSize - footerStartLocation + SPOTIFY_OPUS_HEADER);
  if (!resp->get(cdnUrl, {bell::HTTPClient::RangeHeader::last(footer.size())},
                 false)) {
    return false;
  }

  got = resp->stream().readExact((char*)footer.data(), footer.size());
  resp->stream().close();
  if (got != footer.size()) {
    return false;
  }
  this->decrypt(footer.data(), footer.size(), footerStartLocation);
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

    if (!resp->get(
            cdnUrl,
            {bell::HTTPClient::RangeHeader::range(
                requestPosition, requestPosition + HTTP_BUFFER_SIZE - 1)},
            false)) {
      return 0;
    }
    this->lastRequestPosition = requestPosition;
    this->lastRequestCapacity = resp->contentLength();

    size_t readBytes = resp->stream().readExact((char*)this->httpBuffer.data(),
                                                lastRequestCapacity);
    resp->stream().close();
    this->decrypt(this->httpBuffer.data(), readBytes,
                  this->lastRequestPosition);

    return readBytes(dst, bytes);
  }

  return bytes;
}

#else
/**
 * @brief Opens a connection to the CDN URL and fills the first buffer with track header data.
 *
 * @param header_size Reference to a size_t variable where the size of the header is stored.
 * @return Pointer to the beginning of the HTTP buffer where the track header data is stored.
 */
uint8_t* CDNAudioFile::openStream(ssize_t& header_size) {

  // Open connection, fill first buffer
  auto resp = bell::HTTPClient::get(
      this->cdnUrl,
      {bell::HTTPClient::RangeHeader::range(0, HTTP_BUFFER_SIZE - 1)}, false);
  if (!resp->stream().isOpen() || resp->status() < 200 ||
      resp->status() >= 300) {
    return nullptr;
  }

  this->lastRequestPosition = 0;
  this->lastRequestCapacity = resp->contentLength();
  this->totalFileSize = resp->totalLength();
  size_t got = resp->stream().readExact((char*)this->httpBuffer.data(),
                                        lastRequestCapacity);
  resp->stream().close();
  this->decrypt(this->httpBuffer.data(), got, this->lastRequestPosition);
  this->position = getHeader();
  header_size = this->position;
  return &httpBuffer[0];
}

/**
 * @brief Finds the position of the first audio frame in the HTTP response.
 *
 * The OGG Vorbis file starts with three headers. They contain valuable information
 * for decoding the audio.
 *
 * @return The position of the first audio frame in the HTTP response.
 */
long CDNAudioFile::getHeader() {
  uint32_t offset = SPOTIFY_OPUS_HEADER;

  for (int i = 0; i < 3; ++i) {
    offset += 26;
    if (offset >= HTTP_BUFFER_SIZE) {
      return HTTP_BUFFER_SIZE;
    }
    uint8_t segmentCount = httpBuffer[offset];
    uint32_t segmentEnd = segmentCount + offset + 1;
    ++offset;

    for (uint32_t j = offset; j < segmentEnd; ++j) {
      if (offset >= HTTP_BUFFER_SIZE) {
        return HTTP_BUFFER_SIZE;
      }
      offset += httpBuffer[j];
    }

    if (offset >= HTTP_BUFFER_SIZE) {
      return HTTP_BUFFER_SIZE;
    }
    offset += segmentCount;
  }

  return offset;
}

long CDNAudioFile::readBytes(uint8_t* dst, size_t bytes) {
  if (position + bytes >= this->totalFileSize) {
    if (position >= this->totalFileSize - 1) {
      return 0;
    } else {
      SC32_LOG(info, "Truncating read to %d bytes",
               this->totalFileSize - position);
      bytes = this->totalFileSize - position;
    }
  }
  size_t requestPosition = (position) - ((position) % 16);
  bytes = bytes - bytes % 16;
  if (this->enableRequestMargin) {
    if (response) {
      response->drainBody();
      response->stream().close();
      response.reset();
    }
    this->enableRequestMargin = false;
  }
  if (!response || !response->stream().isOpen()) {
    response = bell::HTTPClient::get(
        cdnUrl, {bell::HTTPClient::RangeHeader::open(requestPosition)}, false);
  }
  size_t got = response->readExact(dst, bytes);
  if (got != bytes) {
    response->drainBody();
    response->stream().close();
    response.reset();
  }
  this->decrypt(dst, got, this->position);
  this->position += got;
  return (long)got;
}
#endif

size_t CDNAudioFile::getSize() {
  return this->totalFileSize;
}

void CDNAudioFile::decrypt(uint8_t* dst, size_t nbytes, size_t pos) {
  if (audioKey.size() != 16 && audioKey.size() != 24 && audioKey.size() != 32) {
    throw std::runtime_error("Invalid AES key length");
  }
  auto calculatedIV = bigNumAdd(audioAESIV, pos / 16);

  this->crypto->aesCTRXcrypt(this->audioKey, calculatedIV, dst, nbytes);
}