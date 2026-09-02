#include "config/config_manager.h"

const char* configPath() {
  return "/config.json";
}

String makeDeviceId() {
  // 保持与原始格式一致: "lamp-" + hex(chipId) 转大写 → "LAMP-XXXXXX"
  char buf[16];
  snprintf(buf, sizeof(buf), "LAMP-%X", (unsigned int)ESP.getChipId());
  return String(buf);
}

bool saveConfig(const DeviceConfig& c) {
  StaticJsonDocument<1024> doc;
  doc["ssid"] = c.ssid;
  doc["password"] = c.password;
  doc["serverHost"] = c.serverHost;
  doc["httpPort"] = c.httpPort;
  doc["wsPort"] = c.wsPort;
  doc["staticIP"] = c.staticIP;
  doc["gateway"] = c.gateway;
  doc["subnetMask"] = c.subnetMask;
  doc["personTrackEnabled"] = c.personTrackEnabled;
  doc["yPerson"] = c.yPerson;
  doc["dRails"] = c.dRails;
  doc["railHeightDiff"] = c.railHeightDiff;
  doc["deadZone"] = c.deadZone;
  doc["ptzDeadZoneDeg"] = c.ptzDeadZoneDeg;   // 2026-08-19 新增: PTZ 推送死区(°)
  doc["standaloneTrackDebug"] = c.standaloneTrackDebug;
  doc["trackDebugInterval"] = c.trackDebugInterval;
  doc["trackAlgorithm"] = c.trackAlgorithm;
  doc["focalPx"] = c.focalPx;
  doc["personHeight"] = c.personHeight;
  doc["faceHeight"] = c.faceHeight;
  doc["hSmoothAlpha"] = c.hSmoothAlpha;
  doc["camHeightMm"] = c.camHeightMm;
  doc["eyeHeightRatio"] = c.eyeHeightRatio;
  doc["chestHeightRatio"] = c.chestHeightRatio;
  doc["tiltOffsetDeg"] = c.tiltOffsetDeg;      // 2026-08-19 新增: 灯云台仰角偏移(°)

  File f = LittleFS.open(configPath(), "w");
  if (!f) return false;

  if (serializeJson(doc, f) == 0) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

bool loadConfig() {
  cfg.ssid = DEFAULT_WIFI_SSID;
  cfg.password = DEFAULT_WIFI_PASSWORD;
  cfg.serverHost = DEFAULT_SERVER_HOST;
  cfg.httpPort = DEFAULT_HTTP_PORT;
  cfg.wsPort = DEFAULT_WS_PORT;
  cfg.staticIP = "";
  cfg.gateway = "";
  cfg.subnetMask = "";
  cfg.personTrackEnabled = false;
  cfg.yPerson = 2000.0f;
  cfg.dRails = 200.0f;
  cfg.railHeightDiff = 200.0f;
  cfg.deadZone = 3.0f;
  cfg.ptzDeadZoneDeg = 1.5f;   // 2026-08-19 新增: PTZ 推送死区(°) 默认值
  cfg.standaloneTrackDebug = false;
  cfg.trackDebugInterval = 200;
  cfg.trackAlgorithm = 0;
  cfg.focalPx = 280.0f;
  cfg.personHeight = 1700.0f;
  cfg.faceHeight = 240.0f;
  cfg.hSmoothAlpha = 0.3f;
  cfg.camHeightMm = 1600.0f; // 2026-08-19 复核修正: 摄像头离地 1600mm (原 1800)
  cfg.eyeHeightRatio = 0.93f;
  cfg.chestHeightRatio = 0.74f;
  cfg.tiltOffsetDeg = 30.0f;   // 2026-08-19 新增: 灯云台仰角偏移(°), 实测仰角偏低补偿

  // 无配置文件：返回 false（语义清晰，表示未从文件加载）
  if (!LittleFS.exists(configPath())) return false;

  File f = LittleFS.open(configPath(), "r");
  if (!f) return false;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  cfg.ssid = doc["ssid"] | DEFAULT_WIFI_SSID;
  cfg.password = doc["password"] | DEFAULT_WIFI_PASSWORD;
  cfg.serverHost = doc["serverHost"] | DEFAULT_SERVER_HOST;
  cfg.httpPort = doc["httpPort"] | DEFAULT_HTTP_PORT;
  cfg.wsPort = doc["wsPort"] | DEFAULT_WS_PORT;
  cfg.staticIP = doc["staticIP"] | "";
  cfg.gateway = doc["gateway"] | "";
  cfg.subnetMask = doc["subnetMask"] | "";
  cfg.personTrackEnabled = doc["personTrackEnabled"] | false;
  cfg.yPerson = doc["yPerson"] | 2000.0f;
  cfg.dRails = doc["dRails"] | 200.0f;
  cfg.railHeightDiff = doc["railHeightDiff"] | 200.0f;
  cfg.railHeightDiff = constrain(cfg.railHeightDiff, -500.0f, 500.0f);
  cfg.deadZone = doc["deadZone"] | 3.0f;
  // 2026-08-19 新增: PTZ 推送死区(°), 旧配置文件无此字段时取默认 1.5, 限幅 [0.3, 5.0]
  cfg.ptzDeadZoneDeg = constrain((float)(doc["ptzDeadZoneDeg"] | 1.5f), 0.3f, 5.0f);
  cfg.standaloneTrackDebug = doc["standaloneTrackDebug"] | false;
  cfg.trackDebugInterval = doc["trackDebugInterval"] | 200;
  cfg.trackAlgorithm = doc["trackAlgorithm"] | 0;
  if (cfg.trackAlgorithm != 1) cfg.trackAlgorithm = 0;
  cfg.focalPx = doc["focalPx"] | 280.0f;
  cfg.personHeight = doc["personHeight"] | 1700.0f;
  cfg.faceHeight = doc["faceHeight"] | 240.0f;
  cfg.faceHeight = constrain(cfg.faceHeight, 100.0f, 500.0f);
  cfg.hSmoothAlpha = doc["hSmoothAlpha"] | 0.3f;
  cfg.camHeightMm = doc["camHeightMm"] | 1600.0f; // 2026-08-19 修正: 旧配置无此字段时回退 1600
  cfg.camHeightMm = constrain(cfg.camHeightMm, 800.0f, 3000.0f);
  cfg.eyeHeightRatio = doc["eyeHeightRatio"] | 0.93f;
  cfg.eyeHeightRatio = constrain(cfg.eyeHeightRatio, 0.85f, 0.98f);
  cfg.chestHeightRatio = doc["chestHeightRatio"] | 0.74f;
  cfg.chestHeightRatio = constrain(cfg.chestHeightRatio, 0.60f, 0.85f);
  // 2026-08-19 新增: 灯云台仰角偏移(°), 旧配置无此字段时取默认 +30, 限幅 ±45
  cfg.tiltOffsetDeg = constrain((float)(doc["tiltOffsetDeg"] | 30.0f), -45.0f, 45.0f);

  return cfg.ssid.length() > 0;
}

void clearConfig() {
  if (LittleFS.exists(configPath())) {
    LittleFS.remove(configPath());
  }
}

void ensureConfigDefaults(DeviceConfig& c) {
  if (c.serverHost.length() == 0) c.serverHost = DEFAULT_SERVER_HOST;
  if (c.httpPort == 0) c.httpPort = DEFAULT_HTTP_PORT;
  if (c.wsPort == 0) c.wsPort = DEFAULT_WS_PORT;
}
