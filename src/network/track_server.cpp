#include "network/track_server.h"
#include "network/lamp_manager.h"
#include "network/camera_upload.h"
#include "device/person_tracker.h"
#include "device/arm_controller.h"
#include "config/config_manager.h"

// ===================== CORS helper =====================
static void addTrackCors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

// ===================== 样板提取：CORS + OPTIONS + body + JSON 解析 =====================
// 返回 true 表示已发送错误响应，调用方应直接 return
// 2026-08-19 改为模板: 各调用方文档容量不同 (384/512), 引用类型需匹配
template <size_t N>
static bool parseTrackBody(StaticJsonDocument<N>& doc) {
  addTrackCors();

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return true;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return true;
  }

  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid JSON\"}");
    return true;
  }

  return false;
}

// ===================== POST /track/start (§5.2) =====================
void handleTrackStart() {
  StaticJsonDocument<384> doc;
  if (parseTrackBody(doc)) return;

  String zone    = doc["zone"] | "";
  String lampId  = doc["lampId"] | "";
  int xAnchor    = doc["xAnchor"] | 0;

  if (zone.length() == 0 || lampId.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"missing zone or lampId\"}");
    return;
  }

  // 注册灯节点（使用请求来源 IP）
  IPAddress lampIP = server.client().remoteIP();
  if (!registerLamp(lampId, zone, xAnchor, lampIP)) {
    server.send(500, "application/json", "{\"error\":\"register failed\"}");
    return;
  }

  // 设为活跃追踪灯
  setActiveTrackingLamp(lampId);

  // 视觉滑轨（对调后为前滑轨，变量名 rear* 保留）移动到 xAnchor
  rearSliderMm = xAnchor;
  sendRearSlider();
  DEBUG_SERIAL.printf("[TRACK] vision slider (front, var=rear) moving to xAnchor=%d mm\n", xAnchor);

  // 启用追踪
  activeTracking = true;
  resetTrackingDebounce();
  resetNearestLamp();   // 新一轮追踪: 就近选灯重新开始

  // 【ESP32-CAM】通知拍照：视觉滑轨到位，拍第一张（服装全景）
  sendCamCmd("Z" + zone);       // 设置区域
  sendCamCmd("Tstart");          // 触发类型=追踪开始
  sendCamCmd("C");               // 拍照

  DEBUG_SERIAL.printf("[TRACK] started: lamp=%s zone=%s xAnchor=%d ip=%s\n",
                      lampId.c_str(), zone.c_str(), xAnchor, lampIP.toString().c_str());

  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"tracking started\"}");
}

// ===================== POST /track/stop (§5.3) =====================
void handleTrackStop() {
  StaticJsonDocument<384> doc;
  if (parseTrackBody(doc)) return;

  String lampId = doc["lampId"] | "";

  if (lampId.length() == 0) {
    server.send(400, "application/json", "{\"error\":\"missing lampId\"}");
    return;
  }

  // 如果这是当前活跃追踪灯，停止追踪
  if (activeLamp.lampId == lampId && activeTracking) {
    activeTracking = false;
    activeLamp = LampNode();
    resetTrackingDebounce();
    resetNearestLamp();   // 清除就近选灯状态, 下次重新选择

    // 【ESP32-CAM】最后一张 + 停止
    sendCamCmd("Tstop");
    sendCamCmd("C");
    sendCamCmd("S");

    DEBUG_SERIAL.printf("[TRACK] stopped by lamp: %s\n", lampId.c_str());
  }

  unregisterLamp(lampId);

  server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"tracking stopped\"}");
}

// ===================== POST /ping (§5.5) =====================
void handleTrackPing() {
  StaticJsonDocument<384> doc;
  if (parseTrackBody(doc)) return;

  String lampId = doc["lampId"] | "";

  if (lampId.length() > 0) {
    updateLampPing(lampId);
  }

  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

// ===================== GET /status (§5.6) =====================
void handleTrackStatus() {
  addTrackCors();

  StaticJsonDocument<768> doc;   // 2026-08-19: 640→768, 新增 tiltOffsetDeg 字段防溢出
  doc["tracking"]     = activeTracking;
  doc["rearPosition"] = rearSliderMm;
  doc["panB"]         = lastTrackResult.pan_B;
  doc["tiltB"]        = lastTrackResult.tilt_B;
  doc["personDetected"] = lastTrackResult.valid;

  if (activeTracking && activeLamp.active) {
    JsonObject lamp = doc.createNestedObject("activeLamp");
    lamp["lampId"]   = activeLamp.lampId;
    lamp["zone"]     = activeLamp.zone;
    lamp["xAnchor"]  = activeLamp.xAnchor;
    lamp["ip"]       = activeLamp.ip.toString();
    lamp["tracking"] = activeLamp.tracking;
  }

  JsonArray lamps = doc.createNestedArray("lamps");
  for (int i = 0; i < lampNodeCount; i++) {
    if (lampNodes[i].active) {
      JsonObject l = lamps.createNestedObject();
      l["lampId"]  = lampNodes[i].lampId;
      l["zone"]    = lampNodes[i].zone;
      l["xAnchor"] = lampNodes[i].xAnchor;
      l["ip"]      = lampNodes[i].ip.toString();
      l["active"]  = lampNodes[i].active;
    }
  }

  doc["yPerson"]      = cfg.yPerson;
  doc["dRails"]       = cfg.dRails;
  doc["railHeightDiff"] = cfg.railHeightDiff;
  doc["deadZone"]     = cfg.deadZone;
  doc["ptzDeadZoneDeg"] = cfg.ptzDeadZoneDeg;   // 2026-08-19 新增: PTZ 推送死区(°)
  doc["trackAlgorithm"] = cfg.trackAlgorithm;
  doc["focalPx"]      = cfg.focalPx;
  doc["personHeight"] = cfg.personHeight;
  doc["faceHeight"]   = cfg.faceHeight;
  doc["hSmoothAlpha"] = cfg.hSmoothAlpha;
  doc["camHeightMm"]      = cfg.camHeightMm;
  doc["eyeHeightRatio"]   = cfg.eyeHeightRatio;
  doc["chestHeightRatio"] = cfg.chestHeightRatio;
  doc["tiltOffsetDeg"] = cfg.tiltOffsetDeg;    // 2026-08-19 新增: 灯云台仰角偏移(°)
  doc["distCam"]      = lastTrackResult.distCam;
  doc["distLamp"]     = lastTrackResult.distLamp;
  doc["hPxRaw"]       = lastTrackResult.hPxRaw;
  doc["hPxSmooth"]    = lastTrackResult.hPxSmooth;
  doc["faceHeightAbs"]  = lastTrackResult.faceHeightAbs;
  doc["chestHeightAbs"] = lastTrackResult.chestHeightAbs;

  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

// ===================== POST /config (§5.6) =====================
void handleTrackConfig() {
  // 2026-08-19: 384→512, 新增 tiltOffsetDeg 后全量配置字段约 430 字节, 384 会静默丢字段
  StaticJsonDocument<512> doc;
  if (parseTrackBody(doc)) return;

  if (doc.containsKey("yPerson"))  cfg.yPerson  = constrain((float)doc["yPerson"],  1000.0f, 5000.0f);
  if (doc.containsKey("dRails"))   cfg.dRails   = constrain((float)doc["dRails"],    200.0f, 1500.0f);
  if (doc.containsKey("railHeightDiff")) cfg.railHeightDiff = constrain((float)doc["railHeightDiff"], -500.0f, 500.0f);
  if (doc.containsKey("deadZone")) cfg.deadZone = constrain((float)doc["deadZone"],    1.0f,   20.0f);
  // 2026-08-19 新增: PTZ 推送死区(°), 限幅 [0.3, 5.0], 修复云台抖动
  if (doc.containsKey("ptzDeadZoneDeg")) cfg.ptzDeadZoneDeg = constrain((float)doc["ptzDeadZoneDeg"], 0.3f, 5.0f);
  if (doc.containsKey("standaloneTrackDebug")) cfg.standaloneTrackDebug = doc["standaloneTrackDebug"] | true;
  if (doc.containsKey("trackDebugInterval"))  cfg.trackDebugInterval  = constrain((int)doc["trackDebugInterval"], 50, 5000);
  bool algoChanged = false;
  if (doc.containsKey("trackAlgorithm")) {
    int a = doc["trackAlgorithm"] | 0;
    a = (a == 1) ? 1 : 0;
    if (a != cfg.trackAlgorithm) algoChanged = true;
    cfg.trackAlgorithm = a;
  }
  if (doc.containsKey("focalPx"))      cfg.focalPx      = constrain((float)doc["focalPx"],      100.0f,  600.0f);
  if (doc.containsKey("personHeight")) cfg.personHeight = constrain((float)doc["personHeight"], 1000.0f, 2200.0f);
  if (doc.containsKey("faceHeight"))   cfg.faceHeight   = constrain((float)doc["faceHeight"],    100.0f,  500.0f);
  if (doc.containsKey("hSmoothAlpha")) cfg.hSmoothAlpha = constrain((float)doc["hSmoothAlpha"],   0.05f,   1.0f);
  if (doc.containsKey("camHeightMm"))      cfg.camHeightMm      = constrain((float)doc["camHeightMm"],      800.0f, 3000.0f);
  if (doc.containsKey("eyeHeightRatio"))   cfg.eyeHeightRatio   = constrain((float)doc["eyeHeightRatio"],   0.85f,  0.98f);
  if (doc.containsKey("chestHeightRatio")) cfg.chestHeightRatio = constrain((float)doc["chestHeightRatio"], 0.60f, 0.85f);
  // 2026-08-19 新增: 灯云台仰角偏移(°), 正值=抬高照射方向, 限幅 ±45
  if (doc.containsKey("tiltOffsetDeg")) cfg.tiltOffsetDeg = constrain((float)doc["tiltOffsetDeg"], -45.0f, 45.0f);

  saveConfig(cfg);

  // 算法模式变更 → 立即应用到 HUSKYLENS (人脸识别=自动 / 物体追踪=按键)
  if (algoChanged) applyHuskyAlgorithm();

  DEBUG_SERIAL.printf("[TRACK] config updated: trackAlgorithm=%d faceHeight=%.0f yPerson=%.0f dRails=%.0f railHeightDiff=%.0f deadZone=%.1f ptzDeadZoneDeg=%.1f focalPx=%.1f personHeight=%.0f hSmoothAlpha=%.2f camHeightMm=%.0f eyeRatio=%.2f chestRatio=%.2f tiltOffset=%.1f\n",
                      cfg.trackAlgorithm, cfg.faceHeight, cfg.yPerson, cfg.dRails, cfg.railHeightDiff, cfg.deadZone, cfg.ptzDeadZoneDeg, cfg.focalPx, cfg.personHeight, cfg.hSmoothAlpha, cfg.camHeightMm, cfg.eyeHeightRatio, cfg.chestHeightRatio, cfg.tiltOffsetDeg);

  String resp;
  StaticJsonDocument<512> respDoc;   // 2026-08-19: 384→512, 15 个字段约 430 字节防溢出
  respDoc["status"] = "ok";
  respDoc["trackAlgorithm"] = cfg.trackAlgorithm;
  respDoc["yPerson"]      = cfg.yPerson;
  respDoc["dRails"]       = cfg.dRails;
  respDoc["railHeightDiff"] = cfg.railHeightDiff;
  respDoc["deadZone"]     = cfg.deadZone;
  respDoc["ptzDeadZoneDeg"] = cfg.ptzDeadZoneDeg;   // 2026-08-19 新增: PTZ 推送死区(°)
  respDoc["focalPx"]      = cfg.focalPx;
  respDoc["personHeight"] = cfg.personHeight;
  respDoc["faceHeight"]   = cfg.faceHeight;
  respDoc["hSmoothAlpha"] = cfg.hSmoothAlpha;
  respDoc["camHeightMm"]      = cfg.camHeightMm;
  respDoc["eyeHeightRatio"]   = cfg.eyeHeightRatio;
  respDoc["chestHeightRatio"] = cfg.chestHeightRatio;
  respDoc["tiltOffsetDeg"] = cfg.tiltOffsetDeg;    // 2026-08-19 新增: 灯云台仰角偏移(°)
  serializeJson(respDoc, resp);
  server.send(200, "application/json", resp);
}

// ===================== POST /track/calibrate (一步标定) =====================
// 原理: focalPx = D_cal × h_px_cal / boxRealHeight
// boxRealHeight 随算法模式变化: 人脸识别=faceHeight, 物体追踪=personHeight
// 人站在已知距离 D_cal 处，用当前检测框 height 反推有效焦距
void handleTrackCalibrate() {
  StaticJsonDocument<384> doc;
  if (parseTrackBody(doc)) return;

  float calibDist = doc["calibDistMm"] | 0.0f;
  if (calibDist < 500.0f || calibDist > 6000.0f) {
    server.send(400, "application/json", "{\"error\":\"calibDistMm out of range [500,6000]\"}");
    return;
  }

  if (!lastTrackResult.valid || lastTrackResult.hPxRaw < H_PX_MIN) {
    server.send(409, "application/json", "{\"error\":\"no valid detection, ensure person is detected first\"}");
    return;
  }

  // 可选：允许标定时一并更新 personHeight / faceHeight
  if (doc.containsKey("personHeight")) {
    cfg.personHeight = constrain((float)doc["personHeight"], 1000.0f, 2200.0f);
  }
  if (doc.containsKey("faceHeight")) {
    cfg.faceHeight = constrain((float)doc["faceHeight"], 100.0f, 500.0f);
  }

  // 标定用的检测框真实高度随算法模式变化:
  // 人脸识别模式 → 人脸框 faceHeight; 物体追踪模式 → 全身框 personHeight
  float boxRealHeight = (cfg.trackAlgorithm == 1) ? cfg.personHeight : cfg.faceHeight;
  cfg.focalPx = calibDist * lastTrackResult.hPxRaw / boxRealHeight;
  saveConfig(cfg);

  float estDist = boxRealHeight * cfg.focalPx / lastTrackResult.hPxRaw;

  StaticJsonDocument<256> resp;
  resp["status"]      = "ok";
  resp["focalPx"]     = cfg.focalPx;
  resp["heightUsed"]  = (int)lastTrackResult.hPxRaw;
  resp["boxHeightMm"] = (int)boxRealHeight;
  resp["estDist"]     = (int)estDist;
  resp["calibDist"]   = (int)calibDist;
  String json;
  serializeJson(resp, json);
  server.send(200, "application/json", json);

  DEBUG_SERIAL.printf("[TRACK] calibrated: focalPx=%.2f hPx=%d boxHeight=%d estDist=%d calibDist=%d\n",
                      cfg.focalPx, (int)lastTrackResult.hPxRaw, (int)boxRealHeight, (int)estDist, (int)calibDist);
}

// ===================== 注册所有路由 =====================
void setupTrackServer() {
  server.on("/track/start", handleTrackStart);
  server.on("/track/stop",  handleTrackStop);
  server.on("/ping",        handleTrackPing);
  server.on("/track/status", handleTrackStatus);
  server.on("/track/config", handleTrackConfig);
  server.on("/track/calibrate", handleTrackCalibrate);

  DEBUG_SERIAL.println(F("[TRACK] HTTP routes registered"));
}
