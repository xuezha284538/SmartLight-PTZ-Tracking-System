#include "network/lamp_notify.h"

// ===================== 向灯节点推送 PTZ 数据 =====================
// 热点优化：每 100ms 被主循环调用一次
// 1. 用 snprintf 直接写静态缓冲，避免 ArduinoJson + String 堆分配
// 2. 复用 WiFiClient + HTTPClient 连接
// 3. setTimeout 限制阻塞
// 4. 仅当 PTZ 数据变化时才推送（节流）
static WiFiClient s_lampClient;
static HTTPClient s_lampHttp;
static bool s_lampHttpActive = false;

// 上次推送的 PTZ 状态（用于变化检测节流）
static float s_lastPanA = 9999.0f;
static float s_lastTiltA = 9999.0f;
static IPAddress s_lastLampIP;

bool notifyLamp(const IPAddress& lampIP, const PersonTrackResult& result) {
  // 变化检测：如果灯 IP 或关键参数都没变，跳过本次推送（减少 ~80% 流量）
  // 2026-08-19 修改: 阈值由硬编码 0.3° 改为可配置死区 cfg.ptzDeadZoneDeg (默认 1.5°)。
  // 0.3° 过小, HUSKYLENS 检测框几像素抖动即超过阈值, 导致每 100ms 推送微小波动角度, 云台抖动。
  // 死区内不推送; 超过死区才推送并刷新基准, 最大稳态误差 = 死区值 (1.5° 对射灯可忽略)。
  bool ipChanged = (s_lastLampIP != lampIP);
  bool ptzChanged = (fabs(result.pan_A - s_lastPanA) > cfg.ptzDeadZoneDeg ||
                     fabs(result.tilt_A - s_lastTiltA) > cfg.ptzDeadZoneDeg);
  if (!ipChanged && !ptzChanged) {
    return true;  // 数据未变化，视为成功（避免重复推送相同数据）
  }

  s_lastPanA = result.pan_A;
  s_lastTiltA = result.tilt_A;
  s_lastLampIP = lampIP;

  // 直接 snprintf 到静态缓冲，避免 ArduinoJson + String 堆分配
  static char jsonBuf[256];
  int len = snprintf(jsonBuf, sizeof(jsonBuf),
    "{\"tracking\":true,\"pan\":%.2f,\"tilt\":%.2f,"
    "\"personData\":{\"xCenter\":%d,\"yCenter\":%d,\"width\":%d,\"height\":%d,"
    "\"pctX\":%.3f,\"deltaX\":%d,\"rearPosition\":%d}}",
    result.pan_A, result.tilt_A,
    result.xCenter, result.yCenter, result.width, result.height,
    result.pctX, (int)result.deltaX, result.rearPosition);

  if (len <= 0 || len >= (int)sizeof(jsonBuf)) return false;

  // 复用 HTTP 连接（IP 变化时需重新 begin）
  if (!s_lampHttpActive || ipChanged) {
    if (s_lampHttpActive) {
      s_lampHttp.end();
    }
    char url[48];
    snprintf(url, sizeof(url), "http://%s/lamp/control", lampIP.toString().c_str());
    s_lampClient.stop();  // 确保旧连接关闭
    s_lampHttp.begin(s_lampClient, url);
    s_lampHttp.addHeader("Content-Type", "application/json");
    s_lampHttp.setTimeout(500);  // 跨设备 HTTP，150ms 太短易超时
    s_lampHttpActive = true;
  }

  int httpCode = s_lampHttp.POST((uint8_t*)jsonBuf, len);
  bool ok = (httpCode == 200);
  if (httpCode > 0) {
    DEBUG_SERIAL.printf("[NOTIFY] lamp %s -> %d (pan=%.1f tilt=%.1f)\n",
                        lampIP.toString().c_str(), httpCode, result.pan_A, result.tilt_A);
  } else {
    DEBUG_SERIAL.printf("[NOTIFY] lamp %s failed: %s\n",
                        lampIP.toString().c_str(), s_lampHttp.errorToString(httpCode).c_str());
    // 失败时关闭连接，下次重试
    s_lampHttp.end();
    s_lampHttpActive = false;
  }

  return ok;
}

// ===================== 通知灯节点停止追踪 =====================
bool notifyLampStop(const IPAddress& lampIP) {
  // 停止通知后重置节流状态，确保下次追踪会推送
  s_lastPanA = 9999.0f;
  s_lastTiltA = 9999.0f;

  // 关闭复用连接（停止可能来自不同 IP）
  if (s_lampHttpActive) {
    s_lampHttp.end();
    s_lampHttpActive = false;
  }

  WiFiClient client;
  HTTPClient http;
  char url[48];
  snprintf(url, sizeof(url), "http://%s/lamp/control", lampIP.toString().c_str());
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(500);

  const char* json = "{\"tracking\":false}";
  int httpCode = http.POST((const uint8_t*)json, strlen(json));
  DEBUG_SERIAL.printf("[NOTIFY] lamp %s stop -> %d\n",
                      lampIP.toString().c_str(), httpCode);
  http.end();

  return (httpCode == 200);
}

// ===================== 清除节流缓存 =====================
void clearNotifyThrottleCache() {
  s_lastPanA = 9999.0f;
  s_lastTiltA = 9999.0f;
  // 关闭复用连接, 确保下次使用新 IP 时重新 begin
  if (s_lampHttpActive) {
    s_lampHttp.end();
    s_lampHttpActive = false;
  }
}

// ===================== 向所有已注册射灯推送 PTZ 坐标 =====================
// 固定向 172.20.10.3 推送（不再遍历 lampIps[]）
void notifyAllLamps(const PersonTrackResult& result) {
  IPAddress ip(172, 20, 10, 3);
  notifyLamp(ip, result);
}

// ===================== 通知所有已注册射灯停止追踪 =====================
// 固定向 172.20.10.3 发送停止（不再遍历 lampIps[]）
void notifyAllLampsStop() {
  IPAddress ip(172, 20, 10, 3);
  notifyLampStop(ip);
}
