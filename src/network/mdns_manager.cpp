#include "network/mdns_manager.h"
#include "app_config.h"
#include <ESP8266mDNS.h>

static bool mdnsStarted = false;

void beginMDNS() {
  if (mdnsStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  // 去除设备 ID 中的特殊字符，确保 mDNS 主机名合法
  String hostname = deviceId;
  hostname.replace(":", "-");
  hostname.replace("_", "-");

  if (MDNS.begin(hostname.c_str())) {
    mdnsStarted = true;
    MDNS.addService("http", "tcp", 80);
    DEBUG_SERIAL.println("[mDNS] started: " + hostname + ".local");
    DEBUG_SERIAL.println("[mDNS] HTTP service advertised on port 80");
  } else {
    DEBUG_SERIAL.println("[mDNS] failed to start");
  }
}

void updateMDNS() {
  if (WiFi.status() != WL_CONNECTED) {
    // WiFi 断连后标记 mDNS 需重新初始化
    if (mdnsStarted) {
      MDNS.end();
      mdnsStarted = false;
    }
    return;
  }

  // 自动恢复：WiFi 重连后重新注册 mDNS
  if (!mdnsStarted) {
    beginMDNS();
    return;
  }

  MDNS.update();
}
