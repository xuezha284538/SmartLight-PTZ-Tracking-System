#pragma once
#include "app_config.h"

// ===================== 拍照/追踪预设 =====================
struct CameraPreset {
  float pan = 0.0f;       // -90..90 度
  float tilt = 0.0f;     // -45..45 度
  int   slider = 0;      // 0..1200 mm
};

// ===================== ROI 区域定义 =====================
struct CameraRoi {
  int    targetIndex = 0;     // 1, 2, 3
  String targetChipId;        // 灯 chipId, 如 "LAMP-001"
  String areaName;            // 区域名称
  float  x = 0.0f;           // ROI 左上角 X (0..1)
  float  y = 0.0f;           // ROI 左上角 Y (0..1)
  float  w = 0.0f;           // ROI 宽度 (0..1)
  float  h = 0.0f;           // ROI 高度 (0..1)
};

// ===================== 灯 HTTP 目标映射 =====================
struct LampHttpTarget {
  int       targetIndex = 0;   // 1, 2, 3
  String    targetChipId;     // 灯 chipId
  IPAddress lampIp;           // 灯 IP (纯 IPv4, 端口固定 80)
  int       xAnchor = -1;     // 灯在滑轨上的安装位置 mm; -1=未提供 (不参与就近选灯)
  bool      valid = false;    // 是否有效
};

// ===================== 摄像头工作状态 =====================
enum CamWorkStatus {
  CAM_MONITORING = 0,
  CAM_CAPTURING,
  CAM_UPLOADING,
  CAM_READY_TRACKING,
  CAM_TRACKING,
  CAM_RETURNING_CENTER,
  CAM_LOST,
  CAM_ERROR
};

// ===================== 拍照任务状态 =====================
enum CaptureTaskState {
  CAPTURE_IDLE = 0,
  CAPTURE_APPLYING_PRESET,
  CAPTURE_TRIGGERING,
  CAPTURE_UPLOADING,
  CAPTURE_DONE,
  CAPTURE_FAILED
};

// ===================== 常量 =====================
static const int MAX_ROIS = 3;
static const char* const ROI_CONFIG_PATH = "/roi_config.json";

// Home 常量定义在 app_config.h 中 (CAM_HOME_PAN/TILT/SLIDER/REAR_SLIDER)

// 拍照运动等待超时
static const unsigned long CAPTURE_PRESET_TIMEOUT_MS = 3000;

// ===================== ROI 配置 (extern) =====================
extern CameraRoi      g_rois[MAX_ROIS];
extern int            g_roiCount;
extern CameraPreset   g_capturePresets[MAX_ROIS];
extern CameraPreset   g_trackingPresets[MAX_ROIS];
extern LampHttpTarget  g_lampTargets[MAX_ROIS];
extern int             g_lampTargetCount;

// 活动目标
extern int             g_activeTargetIndex;       // 0 = 无活动目标
extern String          g_activeTargetChipId;
extern IPAddress       g_activeLampIp;
extern bool            g_activeLampValid;

// 拍照任务
extern CaptureTaskState g_captureState;
extern String           g_captureTaskId;
extern String           g_captureUploadUrl;
extern String           g_captureUploadToken;
extern unsigned long    g_captureStateMs;

// 摄像头工作状态
extern CamWorkStatus g_camWorkStatus;

// ===================== 函数声明 =====================

// ROI 配置加载/保存
bool loadRoiConfig();
bool saveRoiConfig();
void clearRoiConfig();

// 从 JSON 解析 ROI 配置 (校验 + 替换内存配置)
// 返回 true 表示校验通过并已替换; false 表示 JSON 不合法, 保留旧配置
bool applyRoiConfigFromJson(JsonObject& data);

// 灯目标映射管理
bool setLampTargetsFromJson(JsonArray& targets);
LampHttpTarget* findLampTarget(int targetIndex);
LampHttpTarget* findLampTargetByChipId(const String& chipId);
void clearLampTargets();

// 活动目标管理
bool setActiveTarget(int targetIndex, const String& chipId, const IPAddress& lampIp);
void clearActiveTarget();
bool isActiveTargetValid();

// 拍照任务管理
bool startCaptureTask(const String& taskId, const String& uploadUrl,
                      const String& uploadToken, const CameraPreset& preset);
bool isCaptureTaskRunning(const String& taskId);
void finishCaptureTask();
void failCaptureTask();

// 工作状态辅助
const char* camWorkStatusStr(CamWorkStatus s);
