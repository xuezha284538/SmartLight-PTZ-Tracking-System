#include "app_config.h"
#include "config/config_manager.h"
#include "network/wifi_manager.h"
#include "network/http_reporter.h"
#include "network/ws_client.h"
#include "device/sensor_manager.h"
#include "device/ota_manager.h"
#include "device/arm_controller.h"
#include "server/local_server.h"
#include "network/udp_discovery.h"
#include "device/person_tracker.h"
#include "network/track_server.h"
#include "network/lamp_manager.h"
#include "network/lamp_notify.h"
#include "network/mdns_manager.h"
#include "camera/roi_config.h"
#include "network/camera_upload.h"

// ===================== 外设 实例 =====================
WebSocketsClient webSocket;
ESP8266WebServer server(80);
WiFiUDP udp;

// ===================== 运行状态 =====================
bool enableBroadcast = true;
bool enableAnnounce = true;
bool provisioningMode = false;
bool smartConfigActive = false;
bool smartConfigDoneHandled = false;
unsigned long smartConfigStartMs = 0;
bool otaInProgress = false;
String firmwareChannel = FW_CHANNEL;
String otaStatus = "idle";
int otaProgress = 0;
int lastOtaProgressLog = -1;
int lastOtaProgressReport = -1;
unsigned long lastOtaProgressReportMs = 0;

unsigned long lastAnnounce = 0;
unsigned long lastBroadcast = 0;
unsigned long lastPing = 0;

IPAddress cachedBroadcastIP;
bool broadcastIPCached = false;

// ===================== Nano 云台 / 滑轨控制参数 =====================
int panDeg = 0;
int tiltDeg = 0;
int sliderMm = 0;
int angleStep = 5;
int sliderStep = 50;
int panSpeedDeg = 60;
int tiltSpeedDeg = 60;
int sliderSpeedMm = 100;
int rearSliderMm = 600;
int rearStep = 10;

// ===================== 分布式 PTZ 追踪状态 =====================
bool activeTracking = false;
PersonTrackResult lastTrackResult;
unsigned long lastPersonSeenMs = 0;
unsigned long lastTrackPushMs = 0;
unsigned long lastCamCaptureMs = 0;    // 【ESP32-CAM】上次拍照时间
LampNode activeLamp;
LampNode lampNodes[MAX_LAMP_NODES];
int lampNodeCount = 0;

// 服务器后端下发的射灯 IP 列表
String lampIps[8];
int lampIpCount = 0;

// ===================== 设备配置 =====================
DeviceConfig cfg;
String deviceId;

// ===================== setup / loop =====================
void setup() {
  Serial.begin(NANO_BAUD);

  // 初始化独立坐标输出串口 (GPIO2 TX, 仅发送)
  Serial1.begin(TRACK_BAUD);
  delay(200);

  // 清空上电时 ROM bootloader 的垃圾输出 (bootloader 固定 74880 baud)
  while (Serial.available()) Serial.read();
  DEBUG_SERIAL.println();
  deviceId = makeDeviceId();

  DEBUG_SERIAL.println(F("========================"));
  DEBUG_SERIAL.println(F("设备启动"));
  DEBUG_SERIAL.print(F("ID = "));
  DEBUG_SERIAL.println(deviceId);
  DEBUG_SERIAL.print(F("FW = "));
  DEBUG_SERIAL.println(FW_VERSION);
  DEBUG_SERIAL.println(F("========================"));
  DEBUG_SERIAL.flush();

  if (!LittleFS.begin()) {
    DEBUG_SERIAL.println(F("[FS] LittleFS 挂载失败"));
  }

  // 加载 ROI 配置 (设备重启后从 LittleFS 恢复)
  loadRoiConfig();

  setupHardwareAndSensors();

  // 初始化 Nano 云台：细分配置 + 默认速度
  sendNano('m', "16");
  applyArmSpeed("normal");

  bool hasConfig = loadConfig();

  // 按持久化配置应用 HUSKYLENS 算法模式
  // (initPersonTracker 在 loadConfig 之前执行, 用的还是默认值, 这里覆盖一次)
  // 0=人脸识别 → 识别到人自动触发, 无需按键; 1=物体追踪 → 按键学习 (旧行为)
  applyHuskyAlgorithm();

  // 强制 WiFi 凭据: 与 ESP32-S3-CAM 同一局域网 (somebody的iPhone)
  // 覆盖 LittleFS 中可能保存的旧配置 (如 "珺"), 确保两设备在同一网段
  cfg.ssid = DEFAULT_WIFI_SSID;
  cfg.password = DEFAULT_WIFI_PASSWORD;
  DEBUG_SERIAL.printf("[BOOT] 强制 WiFi: %s\n", cfg.ssid.c_str());

  bool wifiOk = false;

  if (hasConfig) {
    DEBUG_SERIAL.println(F("[BOOT] Saved config found, trying to connect..."));
    wifiOk = connectSavedWiFi();
  } else {
    DEBUG_SERIAL.println(F("[BOOT] No saved config, entering parallel provisioning..."));
  }

  if (!wifiOk) {
    DEBUG_SERIAL.println(F("[BOOT] WiFi connection failed — standalone track debug will still work"));
    // 不 return，让 loop() 继续跑独立坐标模式
  }

  broadcastIPCached = false;

  if (wifiOk) {
    beginMDNS();  // 启动 mDNS，可通过 <deviceId>.local 访问
  }

  setupTrackServer();          // 视觉节点 HTTP 路由 (/track/*, /ping)
  setupDeviceHttpServer();
  beginWebSocketClient();
  sendAnnounce();
  sendDeviceStateReport();
}

void loop() {
  server.handleClient();
  pollNano();

  // ===== 独立坐标调试模式 (最优先，不依赖WiFi/HTTP请求) =====
  if (cfg.standaloneTrackDebug && !activeTracking) {
    static unsigned long lastTrackDebugMs = 0;
    static unsigned long lastHuskyHbMs = 0;
    unsigned long now = millis();
    if (now - lastTrackDebugMs >= (unsigned long)cfg.trackDebugInterval) {
      lastTrackDebugMs = now;

      HUSKYLENSResult detected;
      bool personDetected = readPersonFromHuskyLens(detected);

      if (personDetected) {
        // 先按当前滑轨位置做坐标转换 (deltaX/距离与灯位置无关)
        PersonTrackResult r = computeTracking(detected, 600);

        // ===== 就近选灯 (2026-08): 人在哪区, 就近哪盏灯照 =====
        int personX = rearSliderMm + (int)r.deltaX;
        IPAddress lampIp;
        int lampXAnchor = 0;
        bool lampSwitched = false;
        bool haveLamp = selectNearestLamp(personX, lampIp, lampXAnchor, &lampSwitched);

        if (haveLamp) {
          // 灯切换时视觉滑轨跟随到新灯的 xAnchor (保持人在画面中央)
          if (lampSwitched) {
            rearSliderMm = lampXAnchor;
            sendRearSlider();
          }

          // 按选中灯的 xAnchor 重算灯云台A 角度 (视差修正)
          float panA = r.pan_A, tiltA = r.tilt_A;
          computeLampPTZ(lampXAnchor, r, panA, tiltA);
          r.pan_A = panA;
          r.tilt_A = tiltA;

          notifyLamp(lampIp, r);
        } else if (isActiveTargetValid()) {
          // 无注册灯/无带 xAnchor 的目标 → 回退: 推送当前 ROI 绑定的活动灯
          notifyLamp(g_activeLampIp, r);
        }

        DEBUG_SERIAL.printf("[TRACK] ΔX=%.0fmm lampPan=%.1f lampTilt=%.1f distCam=%.0f distLamp=%.0f hPx=%.0f\n",
                            r.deltaX, r.pan_A, r.tilt_A, r.distCam, r.distLamp, r.hPxSmooth);

        // WebSocket 推送坐标到云端 (前端可实时看到)
        // 临时注释：暂不上报 personDetection 坐标到服务器
        // sendPersonDetection(r);

        // 临时注释(2026-08-19)：暂不上报 ROI 检测状态(camPresence)到服务器，与上面 personDetection 同样处理
        // sendCamPresence(1, r.valid ? 0.9f : 0.5f);
      }

      // HUSKYLENS 心跳日志已关闭，避免刷屏
    }
  }

  updateMDNS();  // mDNS 保活（内部检查 WiFi 状态）

  if (provisioningMode) {
    handleProvisioningLoop();
    return;
  }

  if (otaInProgress) return;

  if (!ensureWiFiReady()) return;

  webSocket.loop();

  // 摇杆连续运动更新 (每帧)
  updateArmJoystickMotion();

  broadcastDevice();

  // ===== 分布式追踪循环 (§6.2) =====
  checkLampHeartbeats();

  if (activeTracking) {
    HUSKYLENSResult detected;
    bool personDetected = readPersonFromHuskyLens(detected);

    if (personDetected) {
      // 有人: 坐标转换
      PersonTrackResult r = computeTracking(detected, activeLamp.xAnchor);

      // ===== 就近选灯 (2026-08): 追踪中人也可能跨区, 就近切换 =====
      int personX = rearSliderMm + (int)r.deltaX;
      IPAddress lampIp;
      int lampXAnchor = activeLamp.xAnchor;
      bool lampSwitched = false;
      bool haveLamp = selectNearestLamp(personX, lampIp, lampXAnchor, &lampSwitched);

      if (haveLamp) {
        // 灯切换时视觉滑轨跟随到新灯的 xAnchor
        if (lampSwitched) {
          rearSliderMm = lampXAnchor;
          sendRearSlider();
        }

        // 按选中灯 xAnchor 重算灯云台A 角度
        computeLampPTZ(lampXAnchor, r, r.pan_A, r.tilt_A);
      }
      lastTrackResult = r;

      // 无云台B: 摄像头固定正对人, 检测到人即推灯;
      // 2026-08-19 修改: 灯端 notifyLamp 内置 cfg.ptzDeadZoneDeg (默认1.5°) 死区节流防抖,
      // 灯节点侧另有整数角度变化检测, 双重防抖
      unsigned long now = millis();
      if (now - lastTrackPushMs >= TRACK_PUSH_INTERVAL_MS) {
        lastTrackPushMs = now;

        if (haveLamp) {
          notifyLamp(lampIp, r);
        } else if (isActiveTargetValid()) {
          // 回退: 推送当前 ROI 绑定的活动灯
          notifyLamp(g_activeLampIp, r);
        }

        // 上报追踪状态
        sendTrackingStatus("tracking", r.valid ? 0.9f : 0.5f, "HTTP tracking active");
      }

      DEBUG_SERIAL.printf("[TRACK] ΔX=%.0fmm lampPan=%.1f lampTilt=%.1f\n",
                          r.deltaX, r.pan_A, r.tilt_A);

      lastPersonSeenMs = millis();
    } else {
      // 丢失回中: 3 秒无人 → 停止追踪
      if (millis() - lastPersonSeenMs > TRACK_LOST_TIMEOUT_MS) {
        DEBUG_SERIAL.println("[TRACK] person lost > 3s, stopping");

        // 停止当前活动灯 (不再使用全灯广播停止)
        if (isActiveTargetValid()) {
          notifyLampStop(g_activeLampIp);
        }

        // 视觉滑轨回到固定 Home 位置 (无云台B, 无需摄像头回正)
        rearSliderMm = CAM_HOME_REAR_SLIDER;
        sendRearSlider();

        activeTracking = false;
        resetTrackingDebounce();
        resetNearestLamp();   // 清除就近选灯状态, 下次重新选择

        // 清除活动灯节点和目标
        activeLamp = LampNode();

        // 上报 lost 状态
        g_camWorkStatus = CAM_LOST;
        sendCamStatus(CAM_LOST, "tracking lost");
        sendTrackingStatus("lost", 0.0f, "tracking lost");

        // 停止 ESP32-CAM 追踪
        sendCamCmd("Tstop");
        sendCamCmd("S");

        // 清除活动目标和节流缓存
        clearActiveTarget();
        clearNotifyThrottleCache();

        // 回到 monitoring 状态
        g_camWorkStatus = CAM_MONITORING;
        sendCamStatus(CAM_MONITORING, "back to monitoring");
      }
    }
  }

  // ===== 拍照任务状态机 =====
  if (g_captureState != CAPTURE_IDLE) {
    unsigned long now2 = millis();
    switch (g_captureState) {
      case CAPTURE_APPLYING_PRESET:
        // 等待运动完成或固定超时
        if (now2 - g_captureStateMs >= CAPTURE_PRESET_TIMEOUT_MS) {
          g_captureState = CAPTURE_UPLOADING;
          g_captureStateMs = now2;
          // 上报 uploading 状态
          g_camWorkStatus = CAM_UPLOADING;
          sendCamStatus(CAM_UPLOADING, "uploading photo");
          // 触发 ESP32-CAM 拍照并上传 (通过 TRACK_SERIAL 发送 U/K/C 命令)
          triggerEsp32CamCapture(g_captureUploadUrl, g_captureUploadToken);
        }
        break;

      case CAPTURE_UPLOADING:
        // 等待 ESP32-CAM 完成上传 (固定超时 10 秒)
        if (now2 - g_captureStateMs >= 10000) {
          // 上传完成 (假设成功)
          finishCaptureTask();
          g_camWorkStatus = CAM_MONITORING;
          sendCamStatus(CAM_MONITORING, "capture complete");
        }
        break;

      case CAPTURE_FAILED:
        // 上报错误后回到 monitoring
        if (now2 - g_captureStateMs >= 1000) {
          finishCaptureTask();
          g_camWorkStatus = CAM_MONITORING;
          sendCamStatus(CAM_MONITORING, "capture failed, back to monitoring");
        }
        break;

      default:
        break;
    }
  }

  unsigned long now = millis();

  if (now - lastPing > wsPingInterval) {
    lastPing = now;

    // 用 snprintf 直接构建 JSON，避免 ArduinoJson + String 堆分配
    static char pingBuf[96];
    int len = snprintf(pingBuf, sizeof(pingBuf),
      "{\"type\":\"ping\",\"id\":\"%s\",\"chipId\":\"%s\"}",
      deviceId.c_str(), deviceId.c_str());

    if (len > 0 && len < (int)sizeof(pingBuf)) {
      webSocket.sendTXT(pingBuf, len);
    }
  }

  if (now - lastAnnounce > announceInterval) {
    lastAnnounce = now;
    sendAnnounce();
  }
}
