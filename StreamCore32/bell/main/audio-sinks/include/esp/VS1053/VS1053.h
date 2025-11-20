#ifndef VS1053_H
#define VS1053_H

#include <inttypes.h>
#include <cstring>     //for memset
#include <cmath>
#include <cstdint>
#include <deque>       //for dequeue
#include <functional>  //for function
#include <iostream>
#include <algorithm>
#include <memory>
#include <limits>
#include <optional>
#include <type_traits>

#include "esp_err.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>

#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "vs10xx_uc.h"
#ifdef CONFIG_VS_DSD64
#include "patches_dsd.h"
#endif
#ifdef CONFIG_VS_FLAC
#include "patches_flac.h"
#endif
#ifdef CONFIG_VS_FLAC_LATM
#include "patches_flac_latm.h"
#endif
#ifdef CONFIG_VS_LATM
#include "patches_latm.h"
#endif
#ifdef CONFIG_VS_PITCH
#include "patches_pitch.h"
#endif
#ifdef CONFIG_VS_SPECTRUM_ANALYZER
#include "spectrum_analyzer.h"
#endif

#define VERSION 1
#define VS1053_CHUNK_SIZE 16  // chunck size
#define VS1053_PACKET_SIZE 32
#define BUF_SIZE_CMD 1028
#define BUF_SIZE_FEED 4096 * 4

#define SDI_END_FILL_BYTES_FLAC 12288
#define SDI_END_FILL_BYTES 2050

#define REPORT_INTERVAL 4096 / VS1053_PACKET_SIZE
#define REPORT_INTERVAL_MIDI 512
class VS1053 {
public:
   /**
      * `CONSTRUCTOR`
      *
      * PULLS CS and DCS HIGH.
      * If RESET >= 0, VS1053 gets hardreset.
      * For sharing the SPI_BUS with a SD-Card, the CS-pins
      * of the other devices are needed to be pulled high
      * before mounting the SD-card.
      * After mounting the SD-card, you can add the other devices
      *
      * @param RESET GPIO_PIN_NUM, if
     */
   VS1053(SemaphoreHandle_t* SPI_semaphore);

   /**
      * DECONSTRUCTOR
     */
   ~VS1053();
   class Stream {
   public:
      Stream(void* source, size_t streamId = 0, size_t buffer_size = BUF_SIZE_FEED);
      ~Stream();
      /**
          * feed data to dataBuffer
          * @param data pointer to byte array
          * @param len length of the byte array
          * @warning always call data_request, before feeding data,
          * to get available space in dataBuffer
          */
      size_t feed_data(uint8_t* data, size_t len, bool STORAGE_VOLATILE = 0);
      void empty_feed();
      void run_stream(size_t FILL_BUFFER_BEFORE_PLAYSTART = 0);
      enum State {
         PlaybackStart = 0,
         Playback = 1,
         PlaybackSeekable = 2,
         PlaybackPaused = 3,
         SoftCancel = 4,
         Cancel = 5,
         CancelAwait = 6,
         Stopped = 7
      } state = Stopped;
      size_t header_size = 0;
      size_t streamId;
      StaticStreamBuffer_t xStaticStreamBuffer;
      StreamBufferHandle_t dataBuffer;
      uint8_t* ucBufferStorage;
      void* source = NULL;
   };

   typedef std::function<void(uint8_t)> command_callback;
   /**
      * stops data feed immediately
     */
   void stop_feed();
   /**
      * stops data feed if dataBuffer contains no more
     */
   void soft_stop_feed();
   /**
      * feed data to commandBuffer
      * @param command_callback callback function
      *
     */
   uint8_t feed_command(command_callback commandCallback);
   /**
      * @brief Scales the volume to a logarithmic value
      * @details
      * Given a linear value, this function scales it to a logarithmic
      * value, suitable for use with `set_volume`. The scaling is done
      * according to the formula:
      * `logVolume = 50 * log10(1 + 100 * (value / max))`
      * If the value is not within the range [0, max], it is clamped.
      * If the type `T` is a floating point type, NaN and infinite values
      * are replaced with 0 and 1, respectively.
      *
      * @param value the linear volume value
      * @param limit the maximum value of the linear volume
      * @return the logarithmic volume value
      */
     template<class T>
uint8_t set_volume_logarithmic(T value, T limit) {
    return set_volume_logarithmic<T>(value, std::optional<T>{limit});
}
   template <class T>
   uint8_t to_logarithmic_volume(T value, std::optional<T> limit = std::nullopt) {
      static_assert(std::is_arithmetic<T>::value, "T must be arithmetic");

      T maxv = limit.has_value() ? *limit : std::numeric_limits<T>::max();

      if constexpr (std::is_floating_point<T>::value) {
         if (!std::isfinite(value))  value = T(0);
         if (!std::isfinite(maxv))   maxv = T(1);
      }

      long double v = static_cast<long double>(value);
      long double m = static_cast<long double>(maxv);
      if (m <= 0) m = 1;
      if (v < 0) v = 0;
      if (v > m) v = m;

      // norm to 0..1
      const long double x = v / m;

      long double y = 50.0L * std::log10(1.0L + 100.0L * x);
      if (y < 0)   y = 0;
      if (y > 100) y = 100;

      const uint8_t logVolume = static_cast<uint8_t>(std::llround(y));
      return logVolume;
   }
   template <class T>
   uint8_t to_linear_volume(T value, std::optional<T> limit = std::nullopt) {
      static_assert(std::is_arithmetic<T>::value, "T must be arithmetic");

      T maxv = limit.has_value() ? *limit : std::numeric_limits<T>::max();

      if constexpr (std::is_floating_point<T>::value) {
         if (!std::isfinite(value))  value = T(0);
         if (!std::isfinite(maxv))   maxv = T(1);
      }

      long double v = static_cast<long double>(value);
      long double m = static_cast<long double>(maxv);
      if (m <= 0) m = 1;
      if (v < 0) v = 0;
      if (v > m) v = m;

      // norm to 0..1
      const long double x = v / m;

      long double y = 100.0L * x;
      if (y < 0)   y = 0;
      if (y > 100) y = 100;

      const uint8_t logVolume = static_cast<uint8_t>(std::llround(y));
      return logVolume;
   }

     /**
        * returns the volume, scaled to 0..limit (inclusive) given a logarithmic volume
        * @param logVolume the logarithmic volume (0..100)
        * @param limit the maximum value (default: std::numeric_limits<T>::max())
        * @return the volume, scaled to 0..limit (inclusive)
       */
   template <class T>
   T get_logarithmic_volume(uint8_t logVolume, std::optional<T> limit = std::nullopt) {
      static_assert(std::is_arithmetic<T>::value, "T must be arithmetic");

      // 0..100 clamp
      const long double y = std::clamp<int>(logVolume, 0, 100);

      // set maximum (>= 1)
      long double M = limit.has_value()
         ? static_cast<long double>(*limit)
         : static_cast<long double>(std::numeric_limits<T>::max());
      if (!(M > 0.0L)) M = 1.0L;

      // inverse: x to 0..1
      const long double x = (std::pow(10.0L, y / 50.0L) - 1.0L) / 100.0L;

      // scale to 0..M
      long double val = std::clamp<long double>(x * M, 0.0L, M);

      if constexpr (std::is_floating_point<T>::value) {
         // return as is if floating point or double
         return static_cast<T>(val);
      }
      else {
         // round
         long double rounded = std::floor(val + 0.5L);
         const long double Tmax = static_cast<long double>(std::numeric_limits<T>::max());
         if (rounded > Tmax) rounded = Tmax;
         if (rounded < 0.0L) rounded = 0.0L;
         return static_cast<T>(rounded);
      }
   }
   template <class T>
   T get_linear_volume(uint8_t logVolume, std::optional<T> limit = std::nullopt) {
      static_assert(std::is_arithmetic<T>::value, "T must be arithmetic");

      // 0..100 clamp
      const long double y = std::clamp<int>(logVolume, 0, 100);

      // set maximum (>= 1)
      long double M = limit.has_value()
         ? static_cast<long double>(*limit)
         : static_cast<long double>(std::numeric_limits<T>::max());
      if (!(M > 0.0L)) M = 1.0L;

      // inverse: x to 0..1
      const long double x = y / 100.0L;

      // scale to 0..M
      long double val = std::clamp<long double>(x * M, 0.0L, M);

      if constexpr (std::is_floating_point<T>::value) {
         // return as is if floating point or double
         return static_cast<T>(val);
      }
      else {
         // round
         long double rounded = std::floor(val + 0.5L);
         const long double Tmax = static_cast<long double>(std::numeric_limits<T>::max());
         if (rounded > Tmax) rounded = Tmax;
         if (rounded < 0.0L) rounded = 0.0L;
         return static_cast<T>(rounded);
      }
   }
   /**
      * set volume through cmd_pipeline, sets left and right volume to vol
      * @param vol 0...100, gets capped if bigger
      * @return vol (0...100)
      */
   uint8_t set_volume(uint8_t vol);
   /**
      * set volume through cmd_pipeline, sets separate volume for left and right channel
      * @param left 0...100, gets capped if bigger
      * @param right 0...100, gets capped if bigger
      * @return vol (0...100)
      */
   void set_volume(uint8_t left, uint8_t right);
   /**
      * get available Space in dataBuffer
      * @return free space available in dataBuffer
      */
   size_t data_request(std::shared_ptr<Stream> stream);
   /**
      * loads Usercode(PATCH)
      * @param plugin uint8_t * to plugin array
      * @param sizeofpatch length of the array
      */
   void load_user_code(const unsigned short* plugin, uint16_t sizeofpatch);
   /**
      * test SPI communication and if the board is a VS1053
      * @return `ESP_OK` if setup worked.
      * @return `ESP_ERR_INVALID_RESPONSE` if the register response is wrong.
      * @return `ESP_ERR_NOT_SUPPORTED` if the chip is not a VS1053.
      * @return `ESP_ERR_NOT_FOUND` if the chipNumber is not recognized.
      */
   esp_err_t test_comm(const char* header);

   void run_feed(size_t);

   std::function<void(Stream::State, void* source)> state_callback = nullptr;
   enum Audio_Format {
      afUnknown,
      afRiff,
      afOggVorbis,
      afMp1,
      afMp2,
      afMp3,
      afAacMp4,
      afAacAdts,
      afAacAdif,
      afFlac,
      afWma,
      afMidi,
      afDsd64,
      afLatm
   } audioFormat = afUnknown;

   std::deque<command_callback> command_callbacks;
   std::deque<std::shared_ptr<Stream>> streams;

   void get_audio_format(Audio_Format* audioFormat, size_t* endFillBytes);
   uint16_t read_register(uint8_t _reg);
   bool write_register(uint8_t _reg, uint16_t _value);
   uint32_t read_mem32(uint16_t addr);
   uint32_t read_mem32_counter(uint16_t addr);
   uint16_t read_mem(uint16_t addr);
   void write_mem(uint16_t addr, uint16_t data);
   void write_mem32(uint16_t addr, uint32_t data);
   bool sdi_send_buffer(uint8_t* data, size_t len);
   void remove_stream(std::shared_ptr<Stream> stream) {
      for (int i = 0; i < streams.size(); i++)
         if (streams[i]->streamId == stream->streamId)
            streams.erase(streams.begin() + i);
   };
   void start_stream(std::shared_ptr<Stream>, size_t);
   size_t stream_seekable(size_t);
   void cancel_stream(std::shared_ptr<Stream> stream);
   bool is_cancelled(std::shared_ptr<Stream> stream, uint8_t, size_t);
   size_t get_stream_info(size_t, uint8_t&, size_t&);
   void new_state(std::shared_ptr<Stream>, Stream::State);
   void new_stream(std::shared_ptr<Stream> stream);

   void delete_all_streams(void);
   void setParams(uint32_t sampleRate, uint8_t channelCount, uint8_t bitDepth) {

   }
   size_t spaces_available(size_t);
   size_t command_pointer = 0, command_reader = 0;
   bool isRunning = false;

private:
   spi_device_handle_t SPIHandleLow;
   spi_device_handle_t SPIHandleFast;
   uint8_t curvol;  // Current volume setting 0..100%
   uint16_t __retries = 0;
   int playMode = 0;
   uint8_t chipVersion;  // Version of hardware
   SemaphoreHandle_t* SPI_semaphore = NULL;
   TaskHandle_t VS_TASK;
   void await_data_request();
   bool is_seekable(std::shared_ptr<Stream> stream);
   bool sdi_send_fillers(uint8_t, size_t len);
   void wram_write(uint16_t address, uint16_t data);
   uint16_t wram_read(uint16_t address);
   size_t(*data_callback)(uint8_t*, size_t) = NULL;
   TaskHandle_t task_handle = NULL;
};

#endif