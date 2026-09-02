#include "camera/roi_config.h"

// ===================== 全局变量定义 =====================
CameraRoi      g_rois[MAX_ROIS];
int            g_roiCount = 0;
CameraPreset   g_capturePresets[MAX_ROIS];
CameraPreset   g_trackingPresets[MAX_ROIS];
LampHttpTarget  g_lampTargets[MAX_ROIS];
int            g_lampTargetCount = 0;

int            g_activeTargetIndex = 0;
String         g_activeTargetChipId;
IPAddress      g_activeLampIp;
bool           g_activeLampValid = false;

CaptureTaskState g_captureState = CAPTURE_IDLE;
String           g_captureTaskId;
String           g_captureUploadUrl;
String           g_captureUploadToken;
unsigned long    g_captureStateMs = 0;

CamWorkStatus  g_camWorkStatus = CAM_MONITORING;

// ===================== 工作状态字符串 =====================
const char* camWorkStatusStr(CamWorkStatus s) {
  switch (s) {
    case CAM_MONITORING:        return "monitoring";
    case CAM_CAPTURING:         return "capturing";
    case CAM_UPLOADING:         return "uploading";
    case CAM_READY_TRACKING:    return "ready_tracking";
    case CAM_TRACKING:          return "tracking";
    case CAM_RETURNING_CENTER:  return "returning_center";
    case CAM_LOST:              return "lost";
    case CAM_ERROR:             return "error";
    default:                    return "unknown";
  }
}

// ===================== LittleFS 加载 =====================
bool loadRoiConfig() {
  if (!LittleFS.exists(ROI_CONFIG_PATH)) return false;

  File f = LittleFS.open(ROI_CONFIG_PATH, "r");
  if (!f) return false;

  StaticJsonDocument<1024> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  // 解析 rois 数组
  g_roiCount = 0;
  JsonArray roisArr = doc["rois"].as<JsonArray>();
  for (JsonObject roi : roisArr) {
    if (g_roiCount >= MAX_ROIS) break;
    int idx = roi["targetIndex"] | 0;
    if (idx < 1 || idx > MAX_ROIS) continue;

    g_rois[g_roiCount].targetIndex  = idx;
    g_rois[g_roiCount].targetChipId = roi["targetChipId"] | "";
    g_rois[g_roiCount].areaName     = roi["areaName"] | "";
    g_rois[g_roiCount].x            = roi["x"] | 0.0f;
    g_rois[g_roiCount].y            = roi["y"] | 0.0f;
    g_rois[g_roiCount].w            = roi["w"] | 0.0f;
    g_rois[g_roiCount].h            = roi["h"] | 0.0f;
    g_roiCount++;
  }

  // 解析 capturePresets (key = targetIndex 字符串)
  JsonObject capObj = doc["capturePresets"].as<JsonObject>();
  for (int i = 0; i < g_roiCount; i++) {
    String key = String(g_rois[i].targetIndex);
    if (capObj.containsKey(key)) {
      JsonObject p = capObj[key].as<JsonObject>();
      g_capturePresets[i].pan    = p["pan"] | 0.0f;
      g_capturePresets[i].tilt   = p["tilt"] | 0.0f;
      g_capturePresets[i].slider = p["slider"] | 0;
    }
  }

  // 解析 trackingPresets
  JsonObject trkObj = doc["trackingPresets"].as<JsonObject>();
  for (int i = 0; i < g_roiCount; i++) {
    String key = String(g_rois[i].targetIndex);
    if (trkObj.containsKey(key)) {
      JsonObject p = trkObj[key].as<JsonObject>();
      g_trackingPresets[i].pan    = p["pan"] | 0.0f;
      g_trackingPresets[i].tilt   = p["tilt"] | 0.0f;
      g_trackingPresets[i].slider = p["slider"] | 0;
    }
  }

  DEBUG_SERIAL.printf("[ROI] loaded %d ROI(s) from LittleFS\n", g_roiCount);
  return true;
}

// ===================== LittleFS 保存 =====================
bool saveRoiConfig() {
  StaticJsonDocument<1024> doc;

  JsonArray roisArr = doc.createNestedArray("rois");
  for (int i = 0; i < g_roiCount; i++) {
    JsonObject roi = roisArr.createNestedObject();
    roi["targetIndex"]  = g_rois[i].targetIndex;
    roi["targetChipId"] = g_rois[i].targetChipId;
    roi["areaName"]     = g_rois[i].areaName;
    roi["x"]            = g_rois[i].x;
    roi["y"]            = g_rois[i].y;
    roi["w"]            = g_rois[i].w;
    roi["h"]            = g_rois[i].h;
  }

  JsonObject capObj = doc.createNestedObject("capturePresets");
  for (int i = 0; i < g_roiCount; i++) {
    String key = String(g_rois[i].targetIndex);
    JsonObject p = capObj.createNestedObject(key);
    p["pan"]    = g_capturePresets[i].pan;
    p["tilt"]   = g_capturePresets[i].tilt;
    p["slider"] = g_capturePresets[i].slider;
  }

  JsonObject trkObj = doc.createNestedObject("trackingPresets");
  for (int i = 0; i < g_roiCount; i++) {
    String key = String(g_rois[i].targetIndex);
    JsonObject p = trkObj.createNestedObject(key);
    p["pan"]    = g_trackingPresets[i].pan;
    p["tilt"]   = g_trackingPresets[i].tilt;
    p["slider"] = g_trackingPresets[i].slider;
  }

  File f = LittleFS.open(ROI_CONFIG_PATH, "w");
  if (!f) return false;
  bool ok = (serializeJson(doc, f) > 0);
  f.close();
  return ok;
}

// ===================== 清空 ROI 配置 =====================
void clearRoiConfig() {
  g_roiCount = 0;
  for (int i = 0; i < MAX_ROIS; i++) {
    g_rois[i] = CameraRoi();
    g_capturePresets[i] = CameraPreset();
    g_trackingPresets[i] = CameraPreset();
  }
  if (LittleFS.exists(ROI_CONFIG_PATH)) {
    LittleFS.remove(ROI_CONFIG_PATH);
  }
}

// ===================== 从 JSON 解析 ROI 配置 =====================
bool applyRoiConfigFromJson(JsonObject& data) {
  // 校验 camChipId (必须与本机 deviceId 一致)
  String camChipId = data["camChipId"] | "";
  if (camChipId.length() > 0 && camChipId != deviceId) {
    DEBUG_SERIAL.printf("[ROI] camChipId mismatch: %s != %s\n", camChipId.c_str(), deviceId.c_str());
    return false;
  }

  // 临时数组: 校验全部通过后才替换
  CameraRoi    newRois[MAX_ROIS];
  CameraPreset newCap[MAX_ROIS];
  CameraPreset newTrk[MAX_ROIS];
  int          newCount = 0;

  JsonArray roisArr = data["rois"].as<JsonArray>();
  if (roisArr.isNull()) {
    DEBUG_SERIAL.println("[ROI] rois array missing");
    return false;
  }

  for (JsonObject roi : roisArr) {
    if (newCount >= MAX_ROIS) break;
    int idx = roi["targetIndex"] | 0;
    if (idx < 1 || idx > MAX_ROIS) {
      DEBUG_SERIAL.printf("[ROI] invalid targetIndex: %d\n", idx);
      return false;
    }

    newRois[newCount].targetIndex  = idx;
    newRois[newCount].targetChipId = roi["targetChipId"] | "";
    newRois[newCount].areaName     = roi["areaName"] | "";
    newRois[newCount].x            = constrain((float)(roi["x"] | 0.0f), 0.0f, 1.0f);
    newRois[newCount].y            = constrain((float)(roi["y"] | 0.0f), 0.0f, 1.0f);
    newRois[newCount].w            = constrain((float)(roi["w"] | 0.0f), 0.0f, 1.0f);
    newRois[newCount].h            = constrain((float)(roi["h"] | 0.0f), 0.0f, 1.0f);
    newCount++;
  }

  // 解析 capturePresets
  JsonObject capObj = data["capturePresets"].as<JsonObject>();
  for (int i = 0; i < newCount; i++) {
    String key = String(newRois[i].targetIndex);
    if (capObj.containsKey(key)) {
      JsonObject p = capObj[key].as<JsonObject>();
      newCap[i].pan    = constrain((float)(p["pan"] | 0.0f), -90.0f, 90.0f);
      newCap[i].tilt   = constrain((float)(p["tilt"] | 0.0f), -45.0f, 45.0f);
      newCap[i].slider = constrain((int)(p["slider"] | 0), 0, 1200);
    }
  }

  // 解析 trackingPresets
  JsonObject trkObj = data["trackingPresets"].as<JsonObject>();
  for (int i = 0; i < newCount; i++) {
    String key = String(newRois[i].targetIndex);
    if (trkObj.containsKey(key)) {
      JsonObject p = trkObj[key].as<JsonObject>();
      newTrk[i].pan    = constrain((float)(p["pan"] | 0.0f), -90.0f, 90.0f);
      newTrk[i].tilt   = constrain((float)(p["tilt"] | 0.0f), -45.0f, 45.0f);
      newTrk[i].slider = constrain((int)(p["slider"] | 0), 0, 1200);
    }
  }

  // 校验全部通过, 替换内存配置
  for (int i = 0; i < MAX_ROIS; i++) {
    g_rois[i] = newRois[i];
    g_capturePresets[i] = newCap[i];
    g_trackingPresets[i] = newTrk[i];
  }
  g_roiCount = newCount;

  // 写入 LittleFS
  saveRoiConfig();

  DEBUG_SERIAL.printf("[ROI] applied %d ROI(s), saved to LittleFS\n", g_roiCount);
  return true;
}

// ===================== 灯目标映射管理 =====================
bool setLampTargetsFromJson(JsonArray& targets) {
  clearLampTargets();

  if (targets.isNull()) return true;  // 空数组 → 清空

  for (JsonObject t : targets) {
    if (g_lampTargetCount >= MAX_ROIS) break;

    int idx = t["targetIndex"] | 0;
    if (idx < 1 || idx > MAX_ROIS) continue;

    String chipId = t["targetChipId"] | "";
    String ipStr   = t["lampIp"] | "";

    // 校验: 必须是纯 IPv4, 不能包含 http://, 路径或端口
    if (ipStr.startsWith("http://") || ipStr.startsWith("https://") ||
        ipStr.indexOf('/') >= 0 || ipStr.indexOf(':') >= 0) {
      DEBUG_SERIAL.printf("[LAMP-IP] invalid IP format: %s\n", ipStr.c_str());
      continue;
    }

    IPAddress ip;
    if (!ip.fromString(ipStr)) {
      DEBUG_SERIAL.printf("[LAMP-IP] cannot parse IP: %s\n", ipStr.c_str());
      continue;
    }

    // 可选 xAnchor: 提供时参与就近选灯 (范围 0~2000mm)
    int xAnchor = t["xAnchor"] | -1;
    if (xAnchor >= 0) xAnchor = constrain(xAnchor, 0, 2000);

    g_lampTargets[g_lampTargetCount].targetIndex  = idx;
    g_lampTargets[g_lampTargetCount].targetChipId = chipId;
    g_lampTargets[g_lampTargetCount].lampIp       = ip;
    g_lampTargets[g_lampTargetCount].xAnchor      = xAnchor;
    g_lampTargets[g_lampTargetCount].valid        = true;
    g_lampTargetCount++;

    DEBUG_SERIAL.printf("[LAMP-IP] target %d -> %s (%s) xAnchor=%d\n",
                        idx, chipId.c_str(), ipStr.c_str(), xAnchor);
  }

  return true;
}

LampHttpTarget* findLampTarget(int targetIndex) {
  for (int i = 0; i < g_lampTargetCount; i++) {
    if (g_lampTargets[i].valid && g_lampTargets[i].targetIndex == targetIndex) {
      return &g_lampTargets[i];
    }
  }
  return nullptr;
}

LampHttpTarget* findLampTargetByChipId(const String& chipId) {
  for (int i = 0; i < g_lampTargetCount; i++) {
    if (g_lampTargets[i].valid && g_lampTargets[i].targetChipId == chipId) {
      return &g_lampTargets[i];
    }
  }
  return nullptr;
}

void clearLampTargets() {
  for (int i = 0; i < MAX_ROIS; i++) {
    g_lampTargets[i] = LampHttpTarget();
  }
  g_lampTargetCount = 0;
}

// ===================== 活动目标管理 =====================
bool setActiveTarget(int targetIndex, const String& chipId, const IPAddress& lampIp) {
  if (targetIndex < 1 || targetIndex > MAX_ROIS) {
    DEBUG_SERIAL.printf("[ROI] invalid targetIndex: %d\n", targetIndex);
    return false;
  }

  // 交叉检查: 如果已有灯映射, 验证一致性
  LampHttpTarget* mapped = findLampTarget(targetIndex);
  if (mapped) {
    if (mapped->targetChipId != chipId) {
      DEBUG_SERIAL.printf("[ROI] chipId mismatch: %s != %s\n",
                          chipId.c_str(), mapped->targetChipId.c_str());
      return false;
    }
    // 使用映射中的 IP (确保一致)
    g_activeLampIp = mapped->lampIp;
  } else {
    g_activeLampIp = lampIp;
  }

  g_activeTargetIndex  = targetIndex;
  g_activeTargetChipId = chipId;
  g_activeLampValid    = true;

  DEBUG_SERIAL.printf("[ROI] active target: idx=%d chip=%s ip=%s\n",
                      targetIndex, chipId.c_str(), g_activeLampIp.toString().c_str());
  return true;
}

void clearActiveTarget() {
  g_activeTargetIndex = 0;
  g_activeTargetChipId = "";
  g_activeLampValid = false;
}

bool isActiveTargetValid() {
  return g_activeLampValid && g_activeTargetIndex >= 1;
}

// ===================== 拍照任务管理 =====================
bool startCaptureTask(const String& taskId, const String& uploadUrl,
                      const String& uploadToken, const CameraPreset& preset) {
  if (taskId.length() == 0) {
    DEBUG_SERIAL.println("[CAPTURE] missing taskId");
    return false;
  }

  // 重复 taskId 检查
  if (g_captureState != CAPTURE_IDLE && g_captureTaskId == taskId) {
    DEBUG_SERIAL.printf("[CAPTURE] duplicate taskId: %s, ignoring\n", taskId.c_str());
    return false;
  }

  g_captureTaskId      = taskId;
  g_captureUploadUrl   = uploadUrl;
  g_captureUploadToken = uploadToken;
  g_captureState       = CAPTURE_APPLYING_PRESET;
  g_captureStateMs     = millis();

  DEBUG_SERIAL.printf("[CAPTURE] task started: id=%s url=%s\n",
                      taskId.c_str(), uploadUrl.c_str());
  return true;
}

bool isCaptureTaskRunning(const String& taskId) {
  return (g_captureState != CAPTURE_IDLE && g_captureTaskId == taskId);
}

void finishCaptureTask() {
  DEBUG_SERIAL.printf("[CAPTURE] task finished: %s\n", g_captureTaskId.c_str());
  g_captureState     = CAPTURE_IDLE;
  g_captureTaskId    = "";
  g_captureUploadUrl = "";
  g_captureUploadToken = "";
}

void failCaptureTask() {
  DEBUG_SERIAL.printf("[CAPTURE] task failed: %s\n", g_captureTaskId.c_str());
  g_captureState     = CAPTURE_FAILED;
  g_captureStateMs   = millis();
}
