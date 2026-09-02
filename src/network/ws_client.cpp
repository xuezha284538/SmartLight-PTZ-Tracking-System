#include "network/ws_client.h"
#include "config/config_manager.h"
#include "device/arm_controller.h"
#include "device/ota_manager.h"
#include "network/http_reporter.h"
#include "device/person_tracker.h"
#include "camera/roi_config.h"
#include "network/camera_upload.h"
#include "network/lamp_notify.h"
#include "network/lamp_manager.h"

// 保存 deviceUploadToken 供人流照片上传使用
static String s_deviceUploadToken;
static unsigned long s_trackingSequence = 0;

// ===================== WebSocket 推送人检测坐标 =====================
void sendPersonDetection(const PersonTrackResult& r) {
  if (webSocket.isConnected()) {
    StaticJsonDocument<256> doc;
    doc["type"] = "personDetection";
    doc["chipId"] = deviceId;
    doc["xCenter"] = r.xCenter;
    doc["yCenter"] = r.yCenter;
    doc["width"] = r.width;
    doc["height"] = r.height;
    doc["pctX"] = r.pctX;
    doc["pctY"] = r.pctY;
    doc["deltaX"] = (int)r.deltaX;
    doc["panB"] = r.pan_B;
    doc["tiltB"] = r.tilt_B;
    
    String json;
    serializeJson(doc, json);
    webSocket.sendTXT(json);
    DEBUG_SERIAL.println("[WS] personDetection sent: " + json);
  } else {
    DEBUG_SERIAL.println("[WS] personDetection skipped: not connected");
  }
}

void sendWsRegister() {
  StaticJsonDocument<320> doc;
  doc["type"] = "register";
  doc["id"] = deviceId;
  doc["chipId"] = deviceId;
  doc["deviceType"] = FW_DEVICE_TYPE;
  doc["fwVersion"] = FW_VERSION;
  doc["fwVersionCode"] = FW_VERSION_CODE;
  doc["firmwareChannel"] = FW_CHANNEL;
  doc["otaStatus"] = otaStatus;
  doc["otaProgress"] = otaProgress;
  doc["ip"] = WiFi.localIP().toString();
  doc["mac"] = WiFi.macAddress();

  String msg;
  serializeJson(doc, msg);
  webSocket.sendTXT(msg);
  DEBUG_SERIAL.println("[WS] register: " + msg);
}

// ===================== 上报摄像头工作状态 =====================
void sendCamStatus(CamWorkStatus status, const String& message) {
  if (!webSocket.isConnected()) return;

  StaticJsonDocument<256> doc;
  doc["type"] = "camStatus";
  doc["chipId"] = deviceId;
  doc["workStatus"] = camWorkStatusStr(status);
  if (g_activeTargetIndex > 0) {
    doc["activeTargetIndex"] = g_activeTargetIndex;
    doc["activeTargetChipId"] = g_activeTargetChipId;
  }
  doc["message"] = message;

  String json;
  serializeJson(doc, json);
  webSocket.sendTXT(json);
  DEBUG_SERIAL.println("[WS] camStatus: " + json);
}

// ===================== 上报追踪状态 =====================
void sendTrackingStatus(const char* status, float confidence, const String& message) {
  if (!webSocket.isConnected()) return;

  s_trackingSequence++;

  StaticJsonDocument<320> doc;
  doc["type"] = "trackingStatus";
  doc["chipId"] = deviceId;
  doc["role"] = "cam";
  doc["trackingStatus"] = status;
  doc["camChipId"] = deviceId;
  if (g_activeTargetIndex > 0) {
    doc["lampChipId"] = g_activeTargetChipId;
    doc["targetIndex"] = g_activeTargetIndex;
  }
  doc["confidence"] = confidence;
  doc["sequence"] = s_trackingSequence;
  doc["message"] = message;

  String json;
  serializeJson(doc, json);
  webSocket.sendTXT(json);
}

// ===================== 上报 ROI 检测状态 =====================
void sendCamPresence(int personCount, float confidence) {
  if (!webSocket.isConnected()) return;

  StaticJsonDocument<512> doc;
  doc["type"] = "camPresence";
  doc["chipId"] = deviceId;
  doc["workStatus"] = camWorkStatusStr(g_camWorkStatus);
  doc["personCount"] = personCount;
  doc["confidence"] = confidence;

  JsonArray areas = doc.createNestedArray("areas");
  for (int i = 0; i < g_roiCount; i++) {
    JsonObject area = areas.createNestedObject();
    area["targetIndex"] = g_rois[i].targetIndex;
    area["targetChipId"] = g_rois[i].targetChipId;
    area["areaName"] = g_rois[i].areaName;
    area["present"] = (g_activeTargetIndex == g_rois[i].targetIndex && personCount > 0);
    area["confidence"] = confidence;
  }

  String json;
  serializeJson(doc, json);
  webSocket.sendTXT(json);
  DEBUG_SERIAL.println("[WS] camPresence: " + json);
}

void handleWsMessage(const String& text) {
  DEBUG_SERIAL.println("[WS] 收到消息: " + text);

  StaticJsonDocument<768> doc;
  DeserializationError err = deserializeJson(doc, text);
  if (err) {
    DEBUG_SERIAL.println("[WS] JSON解析失败");
    return;
  }

  JsonObject root = doc.as<JsonObject>();
  JsonObject payload = root;

  if (root["payload"].is<JsonObject>()) {
    payload = root["payload"].as<JsonObject>();
  } else if (root["data"].is<JsonObject>()) {
    payload = root["data"].as<JsonObject>();
  }

  String type = root["type"] | payload["type"] | "";

  if (type == "arm_joystick") {
    float x = payload["x"] | 0.0f;
    float y = payload["y"] | 0.0f;
    int durationMs = payload["durationMs"] | 500;

    setArmJoystickMotion(x, y, durationMs);
    return;
  }

  if (type == "arm_stop") {
    stopArmJoystickMotion();
    return;
  }

  if (type == "arm_position") {
    bool changed = false;

    stopArmJoystickMotion();

    if (payload.containsKey("pan")) {
      panDeg = payload["pan"].as<int>();
      panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
      sendNano('p', String(panDeg));
      changed = true;
    }

    if (payload.containsKey("tilt")) {
      tiltDeg = payload["tilt"].as<int>();
      tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
      sendNano('t', String(tiltDeg));
      changed = true;
    }

    if (payload.containsKey("slider")) {
      sliderMm = payload["slider"].as<int>();
      sliderMm = constrain(sliderMm, SLIDER_MIN, SLIDER_MAX);
      sendNano('x', String(sliderMm));
      changed = true;
    }

    if (!changed) {
      DEBUG_SERIAL.println("[ARM] arm_position missing pan/tilt/slider");
    }

    return;
  }

  if (type == "arm_speed") {
    String speed = payload["speed"] | "normal";
    speed.trim();

    if (speed.length() == 0) {
      speed = "normal";
    }

    applyArmSpeed(speed);
    DEBUG_SERIAL.println("[ARM] speed changed: " + speed);
    return;
  }

  if (type == "arm") {
    String action = payload["action"] | "";
    action.trim();

    if (action.length() == 0) {
      action = payload["direction"] | "";
      action.trim();
    }

    if (action.length() == 0) {
      DEBUG_SERIAL.println("[ARM] missing action");
      return;
    }

    handleArmAction(action);
    return;
  }

  if (type == "command") {
    String cmd = root["cmd"] | payload["cmd"] | "";

    if (cmd == "resume_broadcast" || cmd == "resumeBroadcast") {
      enableBroadcast = true;
      enableAnnounce = true;

      lastBroadcast = 0;
      lastAnnounce = 0;

      DEBUG_SERIAL.println("[WS] resume broadcast command received");
      DEBUG_SERIAL.println("[WS] UDP broadcast resumed");
    } else {
      DEBUG_SERIAL.println("[WS] unknown command: " + cmd);
    }
    return;
  }

  if (type == "person_track") {
    bool enabled = payload["enabled"] | cfg.personTrackEnabled;
    cfg.personTrackEnabled = enabled;
    saveConfig(cfg);
    DEBUG_SERIAL.printf("[TRACK] %s via WebSocket\n", enabled ? "enabled" : "disabled");
    sendDeviceStateReport();
    return;
  }

  if (type == "person_track_config") {
    if (payload.containsKey("yPerson"))  cfg.yPerson  = payload["yPerson"].as<float>();
    if (payload.containsKey("dRails"))   cfg.dRails   = payload["dRails"].as<float>();
    if (payload.containsKey("railHeightDiff")) cfg.railHeightDiff = payload["railHeightDiff"].as<float>();
    if (payload.containsKey("deadZone")) cfg.deadZone = payload["deadZone"].as<float>();
    // 2026-08-19 新增: PTZ 推送死区(°), 限幅 [0.3, 5.0], 修复云台抖动
    if (payload.containsKey("ptzDeadZoneDeg")) cfg.ptzDeadZoneDeg = constrain(payload["ptzDeadZoneDeg"].as<float>(), 0.3f, 5.0f);
    bool algoChanged = false;
    if (payload.containsKey("trackAlgorithm")) {
      int a = payload["trackAlgorithm"].as<int>();
      a = (a == 1) ? 1 : 0;
      if (a != cfg.trackAlgorithm) algoChanged = true;
      cfg.trackAlgorithm = a;
    }
    if (payload.containsKey("focalPx"))      cfg.focalPx      = payload["focalPx"].as<float>();
    if (payload.containsKey("personHeight")) cfg.personHeight = payload["personHeight"].as<float>();
    if (payload.containsKey("faceHeight"))   cfg.faceHeight   = payload["faceHeight"].as<float>();
    if (payload.containsKey("hSmoothAlpha")) cfg.hSmoothAlpha = payload["hSmoothAlpha"].as<float>();
    if (payload.containsKey("camHeightMm"))      cfg.camHeightMm      = payload["camHeightMm"].as<float>();
    if (payload.containsKey("eyeHeightRatio"))   cfg.eyeHeightRatio   = payload["eyeHeightRatio"].as<float>();
    if (payload.containsKey("chestHeightRatio")) cfg.chestHeightRatio = payload["chestHeightRatio"].as<float>();
    // 2026-08-19 新增: 灯云台仰角偏移(°), 正值=抬高照射方向
    if (payload.containsKey("tiltOffsetDeg")) cfg.tiltOffsetDeg = payload["tiltOffsetDeg"].as<float>();
    cfg.yPerson      = constrain(cfg.yPerson,      1000.0f, 5000.0f);
    cfg.dRails       = constrain(cfg.dRails,        200.0f, 1500.0f);
    cfg.railHeightDiff = constrain(cfg.railHeightDiff, -500.0f, 500.0f);
    cfg.deadZone     = constrain(cfg.deadZone,        1.0f,   20.0f);
    cfg.ptzDeadZoneDeg = constrain(cfg.ptzDeadZoneDeg, 0.3f,  5.0f);   // 2026-08-19 新增: PTZ 死区限幅
    cfg.focalPx      = constrain(cfg.focalPx,       100.0f,  600.0f);
    cfg.personHeight = constrain(cfg.personHeight, 1000.0f, 2200.0f);
    cfg.faceHeight   = constrain(cfg.faceHeight,    100.0f,  500.0f);
    cfg.hSmoothAlpha = constrain(cfg.hSmoothAlpha,    0.05f,   1.0f);
    cfg.camHeightMm      = constrain(cfg.camHeightMm,      800.0f, 3000.0f);
    cfg.eyeHeightRatio   = constrain(cfg.eyeHeightRatio,   0.85f,  0.98f);
    cfg.chestHeightRatio = constrain(cfg.chestHeightRatio, 0.60f, 0.85f);
    cfg.tiltOffsetDeg = constrain(cfg.tiltOffsetDeg, -45.0f, 45.0f);   // 2026-08-19 新增: 仰角偏移限幅 ±45
    saveConfig(cfg);

    // 算法模式变更 → 立即应用到 HUSKYLENS
    if (algoChanged) applyHuskyAlgorithm();

    DEBUG_SERIAL.printf("[TRACK] config updated: trackAlgorithm=%d faceHeight=%.0f yPerson=%.0f dRails=%.0f railHeightDiff=%.0f deadZone=%.1f ptzDeadZoneDeg=%.1f focalPx=%.1f personHeight=%.0f hSmoothAlpha=%.2f camHeightMm=%.0f eyeRatio=%.2f chestRatio=%.2f tiltOffset=%.1f\n",
                        cfg.trackAlgorithm, cfg.faceHeight, cfg.yPerson, cfg.dRails, cfg.railHeightDiff, cfg.deadZone, cfg.ptzDeadZoneDeg, cfg.focalPx, cfg.personHeight, cfg.hSmoothAlpha, cfg.camHeightMm, cfg.eyeHeightRatio, cfg.chestHeightRatio, cfg.tiltOffsetDeg);
    return;
  }

  // ===================== cameraRoiConfig: ROI 配置 =====================
  if (type == "cameraRoiConfig") {
    String camChipId = payload["camChipId"] | "";
    if (camChipId.length() > 0 && camChipId != deviceId) {
      DEBUG_SERIAL.println("[ROI] camChipId mismatch, ignoring");
      return;
    }

    JsonObject roiData = payload;
    if (applyRoiConfigFromJson(roiData)) {
      sendCamStatus(g_camWorkStatus, "ROI config updated");
      DEBUG_SERIAL.println("[ROI] config applied successfully");
    } else {
      sendCamStatus(CAM_ERROR, "ROI config validation failed");
      DEBUG_SERIAL.println("[ROI] config validation failed, keeping old config");
    }
    return;
  }

  // ===================== cameraCapture: 拍照任务 =====================
  if (type == "cameraCapture") {
    String taskId      = payload["taskId"] | "";
    String camChipId   = payload["camChipId"] | "";
    String uploadUrl   = payload["uploadUrl"] | "";
    String uploadToken = payload["uploadToken"] | "";
    int    targetIndex = payload["targetIndex"] | 0;
    String targetChipId= payload["targetChipId"] | "";

    // 保存 deviceUploadToken 供人流照片上传使用
    if (payload.containsKey("deviceUploadToken")) {
      s_deviceUploadToken = payload["deviceUploadToken"] | "";
    }

    // 校验 camChipId
    if (camChipId.length() > 0 && camChipId != deviceId) {
      DEBUG_SERIAL.println("[CAM] camChipId mismatch, ignoring capture");
      return;
    }

    // 重复 taskId 检查
    if (isCaptureTaskRunning(taskId)) {
      DEBUG_SERIAL.printf("[CAM] duplicate taskId, ignoring: %s\n", taskId.c_str());
      return;
    }

    // 解析 capturePreset
    CameraPreset preset;
    if (payload.containsKey("capturePreset")) {
      JsonObject p = payload["capturePreset"].as<JsonObject>();
      preset.pan    = constrain((float)(p["pan"] | 0.0f), -90.0f, 90.0f);
      preset.tilt   = constrain((float)(p["tilt"] | 0.0f), -45.0f, 45.0f);
      preset.slider = constrain((int)(p["slider"] | 0), 0, 1200);
    }

    // 启动拍照任务
    if (!startCaptureTask(taskId, uploadUrl, uploadToken, preset)) {
      sendCamStatus(CAM_ERROR, "failed to start capture task");
      return;
    }

    // 上报 capturing 状态
    g_camWorkStatus = CAM_CAPTURING;
    sendCamStatus(CAM_CAPTURING, "capture started");

    // 应用 capturePreset (pan/tilt/slider 通过 Nano 命令下发)
    panDeg = (int)preset.pan;
    tiltDeg = (int)preset.tilt;
    sliderMm = preset.slider;
    sendPanTilt();
    sendNano('x', String(sliderMm));

    DEBUG_SERIAL.printf("[CAM] capture task started: id=%s pan=%.1f tilt=%.1f slider=%d\n",
                        taskId.c_str(), preset.pan, preset.tilt, preset.slider);
    return;
  }

  // ===================== cameraStartTracking: HTTP 追踪命令 =====================
  if (type == "cameraStartTracking") {
    String camChipId   = payload["camChipId"] | "";
    String transport   = payload["transport"] | "";
    int    targetIndex = payload["targetIndex"] | 0;
    String targetChipId= payload["targetChipId"] | "";
    String lampIpStr   = payload["lampIp"] | "";

    // 校验 camChipId
    if (camChipId.length() > 0 && camChipId != deviceId) {
      DEBUG_SERIAL.println("[TRACK] camChipId mismatch, ignoring");
      return;
    }

    // 必须要求 transport == "http"
    if (transport != "http") {
      sendCamStatus(CAM_ERROR, "transport must be http");
      DEBUG_SERIAL.println("[TRACK] invalid transport: " + transport);
      return;
    }

    // 校验 targetIndex
    if (targetIndex < 1 || targetIndex > MAX_ROIS) {
      sendCamStatus(CAM_ERROR, "invalid targetIndex");
      DEBUG_SERIAL.printf("[TRACK] invalid targetIndex: %d\n", targetIndex);
      return;
    }

    // 校验 lampIp 是纯 IPv4, 不能带端口
    if (lampIpStr.startsWith("http://") || lampIpStr.startsWith("https://") ||
        lampIpStr.indexOf('/') >= 0 || lampIpStr.indexOf(':') >= 0) {
      sendCamStatus(CAM_ERROR, "invalid lampIp format");
      DEBUG_SERIAL.println("[TRACK] invalid lampIp: " + lampIpStr);
      return;
    }

    IPAddress lampIp;
    if (!lampIp.fromString(lampIpStr)) {
      sendCamStatus(CAM_ERROR, "cannot parse lampIp");
      return;
    }

    // 用 /lamp-ip 保存的映射交叉检查目标灯
    LampHttpTarget* mapped = findLampTarget(targetIndex);
    if (mapped) {
      if (mapped->targetChipId != targetChipId) {
        sendCamStatus(CAM_ERROR, "targetChipId mismatch with saved mapping");
        DEBUG_SERIAL.printf("[TRACK] chipId mismatch: %s != %s\n",
                            targetChipId.c_str(), mapped->targetChipId.c_str());
        return;
      }
      // 使用映射中的 IP
      lampIp = mapped->lampIp;
    }

    // 设置活动灯
    if (!setActiveTarget(targetIndex, targetChipId, lampIp)) {
      sendCamStatus(CAM_ERROR, "failed to set active target");
      return;
    }

    // 应用 trackingPreset
    if (payload.containsKey("trackingPreset")) {
      JsonObject p = payload["trackingPreset"].as<JsonObject>();
      float pan  = constrain((float)(p["pan"] | 0.0f), -90.0f, 90.0f);
      float tilt = constrain((float)(p["tilt"] | 0.0f), -45.0f, 45.0f);
      int slider = constrain((int)(p["slider"] | 0), 0, 1200);

      panDeg = (int)pan;
      tiltDeg = (int)tilt;
      sliderMm = slider;
      sendPanTilt();
      sendNano('x', String(sliderMm));
    }

    // 清除节流缓存, 确保新追踪会话立即推送
    clearNotifyThrottleCache();

    // 设置 activeLamp 供 computeTracking 使用 xAnchor
    activeLamp.xAnchor = sliderMm;
    activeLamp.ip = lampIp;
    activeLamp.tracking = true;
    activeLamp.active = true;

    // 启动追踪
    activeTracking = true;
    lastPersonSeenMs = millis();
    resetNearestLamp();   // 新一轮追踪: 就近选灯重新开始
    g_camWorkStatus = CAM_TRACKING;

    sendCamStatus(CAM_TRACKING, "HTTP tracking started");
    sendTrackingStatus("tracking", 0.0f, "HTTP tracking started");
    DEBUG_SERIAL.printf("[TRACK] tracking started: target=%d chip=%s ip=%s\n",
                        targetIndex, targetChipId.c_str(), lampIp.toString().c_str());
    return;
  }

  // ===================== cameraReturnCenter: 回中命令 =====================
  if (type == "cameraReturnCenter") {
    String camChipId = payload["camChipId"] | "";

    if (camChipId.length() > 0 && camChipId != deviceId) {
      DEBUG_SERIAL.println("[CENTER] camChipId mismatch, ignoring");
      return;
    }

    // 1. 如果存在活动灯, 向它发送 {"tracking":false}
    if (isActiveTargetValid()) {
      notifyLampStop(g_activeLampIp);
    }

    // 2. 清除活动目标和 lamp_notify 节流缓存
    clearActiveTarget();
    clearNotifyThrottleCache();

    // 3. 停止视觉追踪状态机
    activeTracking = false;
    resetTrackingDebounce();
    resetNearestLamp();   // 清除就近选灯状态
    activeLamp = LampNode();

    // 4. 执行固件内部固定 Home 流程
    g_camWorkStatus = CAM_RETURNING_CENTER;
    sendCamStatus(CAM_RETURNING_CENTER, "returning to center");

    panDeg = CAM_HOME_PAN;
    tiltDeg = CAM_HOME_TILT;
    sendPanTilt();

    sliderMm = CAM_HOME_SLIDER;
    sendNano('x', String(sliderMm));

    rearSliderMm = CAM_HOME_REAR_SLIDER;
    sendRearSlider();

    // 停止 ESP32-CAM 追踪
    sendCamCmd("Tstop");
    sendCamCmd("S");

    // 5. Home 完成后上报 monitoring
    g_camWorkStatus = CAM_MONITORING;
    sendCamStatus(CAM_MONITORING, "return center complete");

    DEBUG_SERIAL.println("[CENTER] return center completed");
    return;
  }

  if (type == "ota_update" || type == "ota:update") {
    String version = payload["version"] | "";
    String url = payload["url"] | "";
    int versionCode = payload["versionCode"] | 0;
    String channel = payload["channel"] | "";
    String md5 = payload["md5"] | "";

    if (channel.length() == 0) {
      channel = FW_CHANNEL;
    }
    channel.toLowerCase();

    if (version.length() == 0 || url.length() == 0 || versionCode <= 0) {
      DEBUG_SERIAL.println("[OTA] OTA message missing version/url/versionCode");
      otaStatus = "failed";
      sendDeviceStateReport();
      return;
    }

    bool sameChannel = (channel == String(FW_CHANNEL));

    if (sameChannel && versionCode <= FW_VERSION_CODE) {
      DEBUG_SERIAL.println("[OTA] Same channel and target versionCode is not newer, ignore");
      otaStatus = "idle";
      otaProgress = 0;
      sendDeviceStateReport();
      return;
    }

    if (!sameChannel) {
      DEBUG_SERIAL.println("[OTA] Cross-channel OTA allowed");
    }

    doOtaUpdate(url, version, versionCode, channel, md5);
    return;
  }

  DEBUG_SERIAL.println("[WS] 未处理消息类型: " + type);
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      DEBUG_SERIAL.println("[WS] 已断开");
      break;

    case WStype_CONNECTED:
      DEBUG_SERIAL.printf("[WS] 已连接: %s\n", payload);
      sendWsRegister();
      break;

    case WStype_TEXT:
      handleWsMessage(String((char*)payload));
      break;

    case WStype_PONG:
      break;

    default:
      break;
  }
}

void beginWebSocketClient() {
  webSocket.disconnect();
  delay(100);

  DEBUG_SERIAL.println("[WS] 准备连接:");
  DEBUG_SERIAL.println("host = " + cfg.serverHost);
  DEBUG_SERIAL.println("port = " + String(cfg.wsPort));
  DEBUG_SERIAL.println("path = " + String(WS_PATH));
  DEBUG_SERIAL.println("url  = ws://" + cfg.serverHost + ":" + String(cfg.wsPort) + String(WS_PATH));

  webSocket.begin(cfg.serverHost.c_str(), cfg.wsPort, WS_PATH);
  webSocket.setExtraHeaders();
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocket.enableHeartbeat(15000, 3000, 2);
}
