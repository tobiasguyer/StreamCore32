#include "MDNSService.h"
#include <arpa/inet.h>
#include <vector>
#include "mdns.h"

using namespace bell;

class implMDNSService : public MDNSService {
 private:
  const std::string type;
  const std::string proto;
  void unregisterService() { mdns_service_remove(type.c_str(), proto.c_str()); }

 public:
  implMDNSService(std::string type, std::string proto)
      : type(type), proto(proto){};
    ~implMDNSService() override {
    esp_err_t err = mdns_service_remove(type.c_str(), proto.c_str());
  }
};

/**
 * ESP32 implementation of MDNSService
 * @see https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/protocols/mdns.html
 **/

std::unique_ptr<MDNSService> MDNSService::registerService(
    const std::string& serviceName, const std::string& serviceType,
    const std::string& serviceProto, const std::string& serviceHost,
    int servicePort, const std::map<std::string, std::string> txtData) {
  std::vector<mdns_txt_item_t> txtItems;
  txtItems.reserve(txtData.size());
  for (auto& kv : txtData) {
    txtItems.push_back(mdns_txt_item_t{ kv.first.c_str(), kv.second.c_str() });
  }
  ESP_ERROR_CHECK(
    mdns_service_add(
      serviceName.c_str(),
      serviceType.c_str(),
      serviceProto.c_str(),
      static_cast<uint16_t>(servicePort),
      txtItems.data(), txtItems.size()
    )
  );

  return std::make_unique<implMDNSService>(serviceType, serviceProto);
}
