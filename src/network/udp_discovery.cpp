#include "network/udp_discovery.h"

IPAddress calcBroadcastIP() {
  IPAddress ip = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  IPAddress broadcast;
  for (int i = 0; i < 4; i++) {
    broadcast[i] = ip[i] | (~mask[i]);
  }
  return broadcast;
}

void refreshBroadcastIP() {
  cachedBroadcastIP = calcBroadcastIP();
  broadcastIPCached = true;
}

void broadcastDevice() {
  if (!enableBroadcast || WiFi.status() != WL_CONNECTED) return;

  if (millis() - lastBroadcast > broadcastInterval) {
    lastBroadcast = millis();

    if (!broadcastIPCached) refreshBroadcastIP();

    // 用 snprintf 直接写静态缓冲，避免多路 String 拼接的堆分配
    static char buf[160];
    String localIp = WiFi.localIP().toString();
    int len = snprintf(buf, sizeof(buf),
      "{\"type\":\"announce\",\"device\":\"%s\",\"id\":\"%s\",\"ip\":\"%s\"}",
      FW_DEVICE_TYPE, deviceId.c_str(), localIp.c_str());

    if (len > 0 && len < (int)sizeof(buf)) {
      udp.beginPacket(cachedBroadcastIP, udpPort);
      udp.write((const uint8_t*)buf, len);
      udp.endPacket();
      DEBUG_SERIAL.print(F("广播: "));
      DEBUG_SERIAL.println(buf);
    }
  }
}
