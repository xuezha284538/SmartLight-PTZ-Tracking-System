#include "network/http_reporter.h"

// URL 缓存：仅在服务器配置变化时重建，避免每次 POST 重复拼接 String
static String cachedServerBase;
static String cachedHttpUrlPrefix;

static void refreshHttpUrlCache() {
  cachedServerBase = cfg.serverHost;
  cachedHttpUrlPrefix = "http://" + cfg.serverHost + ":" + String(cfg.httpPort);
}

String httpUrl(const String& path) {
  if (cachedServerBase != cfg.serverHost) refreshHttpUrlCache();
  return cachedHttpUrlPrefix + path;
}

int postJsonToServer(const String& path, const String& jsonBody) {
  WiFiClient client;
  HTTPClient http;
  http.begin(client, httpUrl(path));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(2000);  // 限制阻塞时长，防止服务器慢导致 loop 卡死
  int httpCode = http.POST(jsonBody);
  if (httpCode > 0) {
    DEBUG_SERIAL.printf("[HTTP] POST %s -> %d\n", path.c_str(), httpCode);
  } else {
    DEBUG_SERIAL.printf("[HTTP] POST %s failed: %s\n", path.c_str(), http.errorToString(httpCode).c_str());
  }
  http.end();
  return httpCode;
}

void sendDeviceStateReport() {
  if (WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<384> doc;
  doc["chipId"] = deviceId;
  doc["deviceType"] = FW_DEVICE_TYPE;
  doc["ip"] = WiFi.localIP().toString();
  doc["firmwareVersion"] = FW_VERSION;
  doc["firmwareVersionCode"] = FW_VERSION_CODE;
  doc["firmwareChannel"] = FW_CHANNEL;
  doc["otaStatus"] = otaStatus;
  doc["otaProgress"] = otaProgress;

  String json;
  serializeJson(doc, json);
  postJsonToServer("/admin/device/state-report", json);
}

void sendAnnounce() {
  if (!enableAnnounce || WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<256> doc;
  doc["chipId"] = deviceId;
  doc["ip"] = WiFi.localIP().toString();
  doc["deviceType"] = FW_DEVICE_TYPE;

  String json;
  serializeJson(doc, json);

  WiFiClient client;
  HTTPClient http;
  http.begin(client, httpUrl("/admin/device/announce"));
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(2000);

  int httpCode = http.POST(json);
  if (httpCode > 0) {
    String payload = http.getString();

    // 用 JSON 解析判断 added 字段（字符串匹配会因空格/字段顺序变化而失效）
    StaticJsonDocument<256> respDoc;
    DeserializationError err = deserializeJson(respDoc, payload);
    if (!err && respDoc["added"] == true) {
      enableAnnounce = false;
      enableBroadcast = false;
      DEBUG_SERIAL.println(F("[HTTP] 成功上报且已添加，停止上报和广播"));
    }
  } else {
    DEBUG_SERIAL.printf("[HTTP] 上报失败: %s\n", http.errorToString(httpCode).c_str());
  }

  http.end();
}

void sendStayRecordToServer(unsigned long durationSeconds) {
  if (WiFi.status() != WL_CONNECTED) return;

  StaticJsonDocument<192> doc;
  doc["chipId"] = deviceId;
  doc["durationValue"] = durationSeconds * 1000UL;

  String payload;
  serializeJson(doc, payload);
  postJsonToServer("/admin/duration/create", payload);
}
