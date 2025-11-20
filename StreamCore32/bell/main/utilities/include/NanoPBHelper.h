#pragma once

#include <stdint.h>  // for uint8_t
#include <stdio.h>   // for printf
#include <string>    // for string
#include <vector>    // for vector

#include "pb.h"         // for pb_msgdesc_t, pb_bytes_array_t, PB_GET_ERROR
#include "pb_decode.h"  // for pb_istream_from_buffer, pb_decode, pb_istream_s
/* For message type T, move src → dst, releasing any old content in dst. */
#define PB_MOVE_ASSIGN(T, dst, src)            \
    do {                                       \
        pb_release(T##_fields, &(dst));        \
        (dst) = (src);                         \
        (src) = T##_init_zero;                 \
    } while (0)

/* If dst is guaranteed to be zero-initialized already, use this: */
#define PB_MOVE_INTO_ZERO(T, dst, src)         \
    do {                                       \
        (dst) = (src);                         \
        (src) = T##_init_zero;                 \
    } while (0)

std::vector<uint8_t> pbEncode(const pb_msgdesc_t* fields,

                              const void* src_struct);

pb_bytes_array_t* vectorToPbArray(const std::vector<uint8_t>& vectorToPack);
pb_bytes_array_t* stringToPbArray(const std::string& stringToPack);
pb_bytes_array_t* charArrayToPbArray(const char* stringToPack);
pb_bytes_array_t* dataToPbArray(const uint8_t* dataToPack, size_t size);


void packString(char*& dst, std::string stringToPack);

std::vector<uint8_t> pbArrayToVector(pb_bytes_array_t* pbArray);
std::vector<uint8_t> pbArrayTToVector(const pb_bytes_array_t* a);

static bool dump_tags(const uint8_t* buf, size_t len) {
    pb_istream_t s = pb_istream_from_buffer(buf, len);
    while (s.bytes_left) {
        uint64_t key;
        if (!pb_decode_varint(&s, &key)) {
            printf("tag read error: %s\n", PB_GET_ERROR(&s));
            return false;
        }
        uint32_t field = (uint32_t)(key >> 3);
        uint32_t wt    = (uint32_t)(key & 7);
        printf("field=%lu wire=%lu\n", field, wt);

        /* skip the field payload so we can continue */
        if (!pb_skip_field(&s, (pb_wire_type_t)wt)) {
            printf("skip error @field %lu: %s\n", field, PB_GET_ERROR(&s));
            return false;
        }
    }
    return true;
}
template <typename T>
T pbDecode(const pb_msgdesc_t* fields, const std::vector<uint8_t>& data) {

  T result = T{};
  // Create stream
  pb_istream_t stream = pb_istream_from_buffer(&data[0], data.size());

  // Decode the message
  if (pb_decode(&stream, fields, &result) == false) {
    printf("Decode failed: %s\n", PB_GET_ERROR(&stream));
    dump_tags(&data[0], data.size()); // zeigt dir die tatsächlichen Tags

  }

  return result;
}
template <typename T>
T pbDecode(const pb_msgdesc_t* fields, const pb_bytes_array_t* data) {

  T result = T{};
  // Create stream
  pb_istream_t stream = pb_istream_from_buffer(data->bytes, data->size);

  // Decode the message
  if (pb_decode(&stream, fields, &result) == false) {
    printf("Decode failed: %s\n", PB_GET_ERROR(&stream));
    dump_tags(&data->bytes[0], data->size); // zeigt dir die tatsächlichen Tags
  }

  return result;
}

template <typename T>
bool pbDecode(T& result, const pb_msgdesc_t* fields,
              std::vector<uint8_t>& data) {
  // Create stream
  pb_istream_t stream = pb_istream_from_buffer(&data[0], data.size());

  // Decode the message
  if (pb_decode(&stream, fields, &result) == false) {
    printf("Decode failed: %s\n", PB_GET_ERROR(&stream));
        dump_tags(&data[0], data.size()); // zeigt dir die tatsächlichen Tags
    return false;
  }
    return true;
}

void pbPutString(const std::string& stringToPack, char* dst);
void pbPutCharArray(const char* stringToPack, char* dst);
void pbPutBytes(const std::vector<uint8_t>& data, pb_bytes_array_t& dst);

const char* pb_encode_to_string(const pb_msgdesc_t* fields, const void* data);
