
#include <MDNSService.h>
#include <arpa/inet.h>
#include <mbedtls/aes.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <memory>
#include <string>
#include "BellHTTPServer.h"
#include "BellLogger.h"  // for setDefaultLogger, AbstractLogger
#include "BellTask.h"
#include "WrappedSemaphore.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mdns.h"
#include "sdkconfig.h"

#include "NvsCreds.h"
#include "ZeroConf.h"
#define StreamCoreFile SecureStore

#include "SecureKeyHelper.h"
#include "SpotifyStream.h"
#include "WebStream.h"

#include "QobuzStream.h"

#include <SpotifyContext.h>
#include <inttypes.h>
#include "BellUtils.h"
#include "DeviceStateHandler.h"
#include "Logger.h"
#include "ZeroConfServer.h"
#include "esp_log.h"

#include "WebUi.h"
#include "lwip/err.h"
#include "lwip/sys.h"

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

#define WIFI_AP_MAXIMUM_RETRY 5

#define DEVICE_NAME CONFIG_SPOTIFY_DEVICE_NAME

#include "AudioControl.h"
extern "C" {
void app_main(void);
}
enum current_streaming_service {
  STREAMING_SERVICE_NONE = 0,
  STREAMING_SERVICE_SPOTIFY,
  STREAMING_SERVICE_QOBUZ,
  STREAMING_SERVICE_RADIO
} current_streaming_service = STREAMING_SERVICE_NONE;
std::function<void()> onEndOfStream;
static int s_retry_num = 0;
std::shared_ptr<SpotifyStream> spotify_app;
std::shared_ptr<QobuzStream> qobuz_app;
std::shared_ptr<AudioControl> audioControl;
std::shared_ptr<AudioControl::FeedControl> feedControl;
std::shared_ptr<WebStream> radio;
std::shared_ptr<bell::BellHTTPServer> httpServer;

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    esp_wifi_connect();
    s_retry_num++;
    ESP_LOGI("WiFi", "retry to connect to the AP");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
    ESP_LOGI("WiFi", "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_num = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

std::function<bool(const std::string&)> WsSendJsonSCLogger =
    [](const std::string& s) -> bool {
  if (WebUI::isConnected()) {
    WebUI::wsSendJson(s);
  }
  return true;
};

void wifi_init_sta(void) {
  s_wifi_event_group = xEventGroupCreate();

  ESP_ERROR_CHECK(esp_netif_init());

  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));
  wifi_config_t wifi_config = {};
  strcpy(reinterpret_cast<char*>(wifi_config.sta.ssid), CONFIG_WIFI_SSID);
  strcpy(reinterpret_cast<char*>(wifi_config.sta.password),
         CONFIG_WIFI_PASSWORD);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI("WiFi", "wifi_init_sta finished.");

  /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
  EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI("WiFi", "connected to ap SSID:%s password:%s", CONFIG_WIFI_SSID,
             CONFIG_WIFI_SSID);
  } else if (bits & WIFI_FAIL_BIT) {
    ESP_LOGI("WiFi", "Failed to connect to SSID:%s, password:%s",
             CONFIG_WIFI_SSID, CONFIG_WIFI_SSID);
  } else {
    ESP_LOGE("WiFi", "UNEXPECTED EVENT");
  }
}

static void sendPlaybackState(mg_connection* conn = nullptr) {
  if (!feedControl)
    return;

  size_t vol = audioControl->volume.load();

  std::string json =
      "{"
      "\"type\":\"playback\","
      "\"volume\":" +
      std::to_string(vol) + "}";

  WebUI::wsSendJson(json, conn);
}
Store radioStore("radio");
static void readWebUIJson(struct mg_connection* conn, char* data, size_t len) {
  if (!len)
    return sendPlaybackState(conn);
  std::string msg(data, len);
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(msg);
  } catch (nlohmann::json::exception& e) {
    SC32_LOG(error, "JSON parse error: %s", e.what());
    return;
  }
  SC32_LOG(info, "WS JSON received: %s", msg.c_str());
  if (j["type"] == "cmd") {
    if (j["cmd"] == "play") {
      if (feedControl)
        feedControl->feedCommand(AudioControl::CommandType::PLAY, 0);
    } else if (j["cmd"] == "pause") {
      if (feedControl)
        feedControl->feedCommand(AudioControl::CommandType::PAUSE, 0);
    } else if (j["cmd"] == "set_volume") {
      if (feedControl)
        feedControl->feedCommand(AudioControl::CommandType::VOLUME_LINEAR,
                                 j["value"].get<uint8_t>(),
                                 std::optional<uint8_t>(100));
    } else if (j["cmd"] == "seek_percent") {
      //if(feedControl) feedControl->feedCommand(AudioControl::CommandType::SEEK_PERCENT, j["value"].get<uint8_t>());
    }
    sendPlaybackState();
    return;
  } else if (j["type"] == "radio.cmd") {
    if (j["cmd"] == "play_station") {
      if (current_streaming_service) {
        onEndOfStream();
      }
      current_streaming_service = STREAMING_SERVICE_RADIO;
      onEndOfStream = []() {
        radio->stop();
        radio = nullptr;
        current_streaming_service = STREAMING_SERVICE_NONE;
      };
      radio = std::make_shared<WebStream>(audioControl);
      radio->onMetadata([](auto st, auto t) {
        nlohmann::json j;
        j["type"] = "playback";
        j["src"] = "Radio";
        std::string artist, title;
        if (t.find(" - ") != std::string::npos) {
          artist = t.substr(0, t.find(" - "));
          title = t.substr(t.find(" - ") + 3);
        } else {
          title = t;
        }
        auto H = radio->getIcyHeaders();
        std::string qlty = H.codec;
        std::string rates = "";
        if (H.bitrateKbps != 0) {
          rates += std::to_string(H.bitrateKbps) + "kbps";
        }
        if (H.sampleRateHz != 0) {
          if (!rates.empty())
            rates += "/";
          rates += std::to_string(H.sampleRateHz) + "Hz";
        }
        if (!rates.empty())
          qlty += " - " + rates;
        j["quality"] = qlty;
        j["track"] = {{"title", title}, {"artist", artist}, {"album", st}};
        WebUI::wsSendJsonStatus(j.dump());
      });
      radio->onError([](auto m) { SC32_LOG(error, "%s", m.c_str()); });
      std::string url = j["station"]["url"].get<std::string>();
      std::string name = j["station"]["name"].get<std::string>();
      radio->play(url, name);
      current_streaming_service = STREAMING_SERVICE_RADIO;
    } else if (j["cmd"] == "save_station") {
      Record r;
      radioStore.load("stations", &r);
      for (auto& s : r.fields) {
        if (s.name == j["station"]["name"].get<std::string>()) {
          s = {j["station"]["name"].get<std::string>(),
               j["station"]["url"].get<std::string>()};
          radioStore.save(r);
          return;
        }
      }
      if (r.userkey.empty())
        r.userkey = "stations";
      r.fields.push_back({j["station"]["name"].get<std::string>(),
                          j["station"]["url"].get<std::string>()});
      radioStore.save(r);
    } else if (j["cmd"] == "remove_station") {
      Record r;
      radioStore.load("stations", &r);
      std::string name = j["station"]["name"].get<std::string>();
      for (auto it = r.fields.begin(); it != r.fields.end(); it++) {
        if (it->name == name) {
          r.fields.erase(it);
          radioStore.save(r);
          return;
        }
      }
    }
  } else if (j["type"] == "page") {
    if (j["page"] == "page-radio") {
      nlohmann::json j;
      j["type"] = "radio";
      j["cmd"] = "stations";
      Record stations;
      radioStore.load("stations", &stations);
      j["stations"] = {};
      for (auto& s : stations.fields) {
        j["stations"].push_back(
            {{"name", s.name},
             {"url", std::string(s.value.begin(), s.value.end())}});
      }
      WebUI::wsSendJson(j.dump());
    } else if (j["page"] == "page-debug") {
      nlohmann::json j;
      j["type"] = "debug";
      wifi_ap_record_t info{};
      if (esp_wifi_sta_get_ap_info(&info) == ESP_OK) {
        j["rssi"] = info.rssi;
        j["heap"] = esp_get_free_heap_size() / 1024;
        UBaseType_t taskCount = uxTaskGetNumberOfTasks();
        std::vector<TaskStatus_t> tasks(taskCount);
        uint32_t totalRunTime;
        taskCount =
            uxTaskGetSystemState(tasks.data(), taskCount, &totalRunTime);

        std::string out;
        bool first = true;
        j["tasks"] = {};
        for (UBaseType_t i = 0; i < taskCount; ++i) {
          j["tasks"].push_back({{"task", tasks[i].pcTaskName},
                                {"state", tasks[i].eCurrentState},
                                {"priority", tasks[i].uxCurrentPriority},
                                {"stack", tasks[i].usStackHighWaterMark}});
        }
      }
      WebUI::wsSendJson(j.dump());
    }
  }
}
void app_main(void) {
  init_nvs();
  esp_err_t ret;
  // SPI SETUP
  spi_bus_config_t bus_cfg;
  memset(&bus_cfg, 0, sizeof(spi_bus_config_t));
  bus_cfg.sclk_io_num = CONFIG_GPIO_CLK;
  bus_cfg.mosi_io_num = CONFIG_GPIO_MOSI;
  bus_cfg.miso_io_num = CONFIG_GPIO_MISO;
  bus_cfg.quadwp_io_num = -1;
  bus_cfg.quadhd_io_num = -1;
  ESP_LOGI("vsInit", "spi config done");
  ret = spi_bus_initialize(HSPI_HOST, &bus_cfg, 1);
  assert(ret == ESP_OK);
#if defined(SD_IN_USE)
  initSD();

#endif
  wifi_init_sta();

  ESP_LOGI("MAIN", "Connected to AP, start spotify receiver");

  bell::setDefaultLogger();
  mdns_init();
  mdns_hostname_set("sc32");
  mdns_instance_name_set("StreamCore32");
  InitZeroconf("StreamCore32", 7864);
  bell::MDNSService::registerService("StreamCore32", "_http", "_tcp", "", 80,
                                     {
                                         {"Name", "StreamCore32"},
                                     });
  audioControl = std::make_shared<AudioControl>();
  feedControl = std::make_unique<AudioControl::FeedControl>(audioControl);
  WebUI::WebUI_start(80, readWebUIJson);
  timesync::init();
  timesync::set_timezone_ch();
  if (!timesync::wait_until_valid(8000)) {
    BELL_LOG(error, "QOBUZ",
             "System time not valid; cannot call API needing request_ts");
  }
  qobuz_app = std::make_shared<QobuzStream>(
      audioControl, QobuzStream::Config{},
      std::make_unique<SecureStore>("qobuz"), [](bool connected) {
        if (connected) {
          if (current_streaming_service) {
            onEndOfStream();
          }
          current_streaming_service = STREAMING_SERVICE_QOBUZ;
          onEndOfStream = []() {
            qobuz_app->stop();
            current_streaming_service = STREAMING_SERVICE_NONE;
          };
        } else if (current_streaming_service == STREAMING_SERVICE_QOBUZ) {
          current_streaming_service = STREAMING_SERVICE_NONE;
        }
      });
  qobuz_app->onUiMessage_ = [](const std::string& msg) {
    WebUI::wsSendJsonStatus(msg);
  };
  spotify_app = std::make_unique<SpotifyStream>(
      audioControl, std::make_unique<SecureStore>("spotify"),
      [](bool connected) {
        if (connected) {
          if (current_streaming_service) {
            onEndOfStream();
          }
          current_streaming_service = STREAMING_SERVICE_SPOTIFY;
          onEndOfStream = []() {
            spotify_app->stop();
            current_streaming_service = STREAMING_SERVICE_NONE;
          };
        } else if (current_streaming_service == STREAMING_SERVICE_SPOTIFY) {
          current_streaming_service = STREAMING_SERVICE_NONE;
        }
      });
  spotify_app->onUiMessage_ = [](const std::string& msg) {
    WebUI::wsSendJsonStatus(msg);
  };
  vTaskSuspend(NULL);
}