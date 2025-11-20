#include "ZeroConf.h"

ZeroconfServiceManager zeroconf;
bool InitZeroconf(const std::string& deviceName, int port){
  return zeroconf.initialize(deviceName, port);
};