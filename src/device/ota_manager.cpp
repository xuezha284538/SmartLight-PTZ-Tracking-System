#include "device/ota_manager.h"
#include "network/http_reporter.h"
#include "network/ws_client.h"

// ---- OTA 回调 ----
// 注意：ESPhttpUpdate.update() 是阻塞调用，回调在其中执行。
// 不在回调中发 HTTP 上报（堆紧张 + WiFi 栈不稳定 + 阻塞 OTA 进度），
// 仅更新 otaProgress 全局变量 + 串口日志。OTA 完成后在 switch 分支统一上报。

static void otaStarted() {
  DEBUG_SERIAL.println(F("[OTA] 开始升级"));
}

static void otaFinished() {
  DEBUG_SERIAL.println(F("[OTA] 升级完成"));
}

static void otaProgressCallback(int current, int total) {
  if (total <= 0) return;

  int percent = (current * 100) / total;
  percent = constrain(percent, 0, 100);

  otaProgress = percent;

  if (lastOtaProgressLog < 0 || percent - lastOtaProgressLog >= 5 || percent == 100) {
    lastOtaProgressLog = percent;
    DEBUG_SERIAL.printf("[OTA] progress %d%% (%d/%d)\n", percent, current, total);
  }

  yield();
}

static void otaError(int err) {
  DEBUG_SERIAL.printf("[OTA] 错误码: %d\n", err);
}

// ---- OTA 执行 ----

void doOtaUpdate(const String& url, const String& version, int versionCode, const String& channel, const String& md5) {
  if (otaInProgress) return;
  otaInProgress = true;
  otaStatus = "updating";
  otaProgress = 0;
  lastOtaProgressLog = -1;
  lastOtaProgressReport = -1;
  lastOtaProgressReportMs = 0;
  firmwareChannel = FW_CHANNEL;
  sendDeviceStateReport();

  DEBUG_SERIAL.println(F("[OTA] 收到升级通知"));
  DEBUG_SERIAL.println("[OTA] 当前版本: " + String(FW_VERSION));
  DEBUG_SERIAL.println("[OTA] 目标版本: " + version);
  DEBUG_SERIAL.println("[OTA] 下载地址: " + url);

  webSocket.disconnect();
  delay(200);

  WiFiClient client;
  ESPhttpUpdate.setClientTimeout(12000);
  ESPhttpUpdate.rebootOnUpdate(false);
  ESPhttpUpdate.onStart(otaStarted);
  ESPhttpUpdate.onEnd(otaFinished);
  ESPhttpUpdate.onProgress(otaProgressCallback);
  ESPhttpUpdate.onError(otaError);
  if (md5.length() > 0) {
    ESPhttpUpdate.setMD5sum(md5.c_str());
  }

  t_httpUpdate_return ret = ESPhttpUpdate.update(client, url);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      DEBUG_SERIAL.printf("[OTA] 升级失败 (%d): %s\n",
                    ESPhttpUpdate.getLastError(),
                    ESPhttpUpdate.getLastErrorString().c_str());
      otaStatus = "failed";
      sendDeviceStateReport();
      otaInProgress = false;
      beginWebSocketClient();
      break;

    case HTTP_UPDATE_NO_UPDATES:
      otaStatus = "idle";
      otaProgress = 0;
      sendDeviceStateReport();
      DEBUG_SERIAL.println(F("[OTA] 没有更新"));
      otaInProgress = false;
      beginWebSocketClient();
      break;

    case HTTP_UPDATE_OK:
      otaStatus = "success";
      otaProgress = 100;
      sendDeviceStateReport();
      delay(300);
      DEBUG_SERIAL.println(F("[OTA] 升级成功，设备将自动重启"));
      ESP.restart();
      break;
  }
}

void doOtaUpdate(const String& url, const String& version) {
  doOtaUpdate(url, version, 0, FW_CHANNEL, "");
}

// ---- 版本比较 ----

int compareVersion(const String& a, const String& b) {
  int a1 = 0, a2 = 0, a3 = 0;
  int b1 = 0, b2 = 0, b3 = 0;
  sscanf(a.c_str(), "%d.%d.%d", &a1, &a2, &a3);
  sscanf(b.c_str(), "%d.%d.%d", &b1, &b2, &b3);

  if (a1 != b1) return (a1 > b1) ? 1 : -1;
  if (a2 != b2) return (a2 > b2) ? 1 : -1;
  if (a3 != b3) return (a3 > b3) ? 1 : -1;
  return 0;
}
