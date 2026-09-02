#pragma once

// ===================== 系统 Includes =====================
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266httpUpdate.h>
#include <LittleFS.h>
#include <Wire.h>

// ===================== 串口别名 =====================
// Serial(GPIO1/GPIO3) = USB 串口 + Nano 通信 (57600 baud)
// Serial1(GPIO2) = 独立坐标输出 TX (115200 baud, 仅发送)
#define nanoSerial Serial
#define DEBUG_SERIAL Serial
#define TRACK_SERIAL Serial1     // 独立坐标输出串口

// ===================== 引脚定义 =====================
#define TOF_SDA_PIN  D5
#define TOF_SCL_PIN  D6
#define HUSKYLENS_I2C_ADDR 0x32

// ===================== 固件信息 =====================
#define FW_DEVICE_TYPE  "cam"
#define FW_VERSION      "1.2.0"
#define FW_VERSION_CODE 10200
#define FW_CHANNEL      "stable"

// ===================== 命名常量 =====================
const char* const AP_DEFAULT_PASSWORD   = "12345678";
const char* const WS_PATH               = "/ws/device";

// ===================== 默认服务器配置 =====================
const char* const DEFAULT_SERVER_HOST = "device.genius.show";
const uint16_t DEFAULT_HTTP_PORT = 80;
const uint16_t DEFAULT_WS_PORT   = 80;
const char* const DEFAULT_WIFI_SSID = "somebody的iPhone";
const char* const DEFAULT_WIFI_PASSWORD = "20040000";

// ===================== ESP32-S3-CAM 配置 =====================
// ESP32-S3-CAM 固定 IP (与 8266 同一局域网, 通过 HTTP /cmd 接口触发拍照)
// 通信方式: 8266 --(HTTP)--> ESP32-S3-CAM, 替代原 Serial2 串口通信
const char* const ESP32_CAM_HOST ="172.20.10.4";
const uint16_t    ESP32_CAM_PORT = 80;

// ===================== 定时参数 =====================
const unsigned long wifiConnectTimeout   = 15000;
const unsigned long smartConfigTimeout   = 30000;

const unsigned long announceInterval     = 5000;
const unsigned long broadcastInterval    = 5000;
const unsigned long wsPingInterval       = 5000;
const unsigned long otaProgressReportMinIntervalMs = 3000;
const int           otaProgressReportMinStep       = 5;

const int udpPort = 4210;
const uint32_t NANO_BAUD = 57600;
const uint32_t TRACK_BAUD = 115200;   // 独立坐标输出串口波特率

// ===================== 分布式 PTZ 追踪常量 =====================
const unsigned long TRACK_PUSH_INTERVAL_MS   = 100;   // PTZ 数据推送间隔
const unsigned long LAMP_HEARTBEAT_INTERVAL_MS = 5000; // 灯心跳间隔
const unsigned long LAMP_HEARTBEAT_TIMEOUT_MS  = 15000;// 灯心跳超时
const unsigned long TRACK_LOST_TIMEOUT_MS      = 3000; // 人消失超时
const int           MAX_LAMP_NODES             = 3;    // 最大灯节点数
const unsigned long TRACK_DEBUG_INTERVAL_MS    = 200;  // 独立调试模式输出间隔

// 就近选灯滞回: 新灯需比当前灯近该距离以上才切换, 防止人在两区交界处来回跳灯
const int NEAREST_LAMP_HYSTERESIS_MM = 150;

// 2.5.0: 固定角偏移 TILT_BASE/CHEST_OFFSET 已删除。
// 射灯瞄准改为绝对高度模型: 由摄像头离地高度 + 人脸位置反推胸口绝对高度,
// 见 person_tracker.cpp computeTracking() Step 5 与 SYSTEM_DESIGN.md §6.4。

// ===================== 固定 Home 位置常量 (不由前端配置) =====================
const int CAM_HOME_PAN         = 0;
const int CAM_HOME_TILT        = 0;
const int CAM_HOME_SLIDER      = 0;
const int CAM_HOME_REAR_SLIDER = 800;  // 前滑轨回中 800mm (行程 0~1600 的中点, 与中区灯 xAnchor 对齐)

// HUSKYLENS 光学参数
const int   HUSKY_FRAME_W  = 320;
const int   HUSKY_FRAME_H  = 240;
const int   HUSKY_FRAME_CX = 160;
const int   HUSKY_FRAME_CY = 120;
const float HUSKY_FOV_H    = 60.0f;   // 水平视场角(°)
const float HUSKY_FOV_V    = 45.0f;   // 垂直视场角(°)
const float TAN_HALF_FOV_H = 0.57735f; // tan(30°)
const float RAD2DEG        = 57.29578f; // 180/π，弧度转角度（避开 Arduino.h 的 RAD_TO_DEG 宏）

// ===================== height 自适应测距常量 =====================
const float D_MIN_MM        = 800.0f;    // 测距下限(mm)
const float D_MAX_MM        = 6000.0f;   // 测距上限(mm)
const float D_MIN_LAMP_MM   = 300.0f;    // 灯深度下限, 防负深度
const int   H_PX_MIN        = 20;        // height 像素下限
const int   H_PX_MAX        = 240;       // height 像素上限(满画面)
const float H_OUTLIER_RATIO = 0.35f;     // height 帧间突变阈值(35%)

// ===================== DeviceConfig =====================
struct DeviceConfig {
  String ssid = DEFAULT_WIFI_SSID;
  String password = DEFAULT_WIFI_PASSWORD;
  String serverHost;
  uint16_t httpPort;
  uint16_t wsPort;
  // 静态 IP（空字符串 = 使用 DHCP）
  String staticIP;
  String gateway;
  String subnetMask;
  // 人像跟踪
  bool personTrackEnabled = false;
  float yPerson = 2000.0f;
  float dRails = 200.0f;    // 前后滑轨间距 mm (§4.4)
  float railHeightDiff = 200.0f; // 后滑轨(灯)相对前滑轨(摄像头)高度差 mm, 正值=灯更高 (实际安装前滑轨低 20cm)
  float deadZone = 3.0f;    // deprecated: 无云台B 场景不再使用, 保留以兼容配置 API
  // 2026-08-19 新增: PTZ 推送死区(°), 修复云台抖动。
  // notifyLamp 仅当 pan/tilt 相对上次推送变化超过该值时才向灯节点发 HTTP,
  // 滤除 HUSKYLENS 检测框像素抖动造成的微小角度波动 (旧硬编码阈值 0.3° 过小)。
  float ptzDeadZoneDeg = 1.5f;
  bool standaloneTrackDebug = true;  // 独立调试模式: 无需HTTP请求即输出坐标
  int  trackDebugInterval = 200;     // 独立模式输出间隔 (ms)
  // HUSKYLENS 算法模式 (2026-08 自动触发改造, wiki §7.2 → 人脸识别)
  // 0 = 人脸识别: 识别到人(含未学习人脸 ID=0)即自动输出坐标, 无需按键  ← 默认
  // 1 = 物体追踪: 需在二哈上按键学习目标后才输出 (旧行为)
  int   trackAlgorithm = 0;
  // height 自适应测距参数
  float focalPx       = 280.0f;   // 有效焦距(像素), 一步标定得出
  float personHeight  = 1700.0f;  // 假定人真实身高(mm), 物体追踪模式(全身框)测距用
  float faceHeight    = 240.0f;   // 人脸框真实高度(mm), 人脸识别模式测距用
  float hSmoothAlpha  = 0.3f;     // height EMA 平滑系数(0~1, 越小越平滑)
  // 绝对高度瞄准模型 (2.5.0: 射灯照胸口)
  // 人高矮/站位不固定: 距离由人脸框在线测量, 胸口点高度由人脸高度按人体比例反推
  float camHeightMm      = 1600.0f; // 前滑轨(摄像头)离地高度 mm (2026-08-19 复核修正, 原 1800); 灯离地 = camHeightMm + railHeightDiff (1800mm)
  float eyeHeightRatio   = 0.93f;   // 人脸框中心(眼位)高度 ≈ 0.93 × 身高 (人体测量学)
  float chestHeightRatio = 0.74f;   // 胸口照射点高度 ≈ 0.74 × 身高 (人体测量学)
  // 2026-08-19 新增: 灯云台仰角偏移(°), 正值=抬高照射方向。
  // 实测云台仰角偏低(光斑照偏下), 用户要求直接 +30° 补偿(灯端机械零位/安装偏差)。
  // 最终 tilt_A = 几何计算值 + tiltOffsetDeg, 限幅 ±45°; 可通过 /track/config 在线调
  float tiltOffsetDeg = 30.0f;
  // yPerson 保留作降级备用
};

// ===================== 坐标转换结果 =====================
struct PersonTrackResult {
  // 归一化偏移
  float pctX = 0.0f;        // 水平归一化偏移 [-1, +1]
  float pctY = 0.0f;        // 垂直归一化偏移 [-1, +1]
  // 物理偏移
  float deltaX = 0.0f;      // 物理水平偏移 (mm)
  // 云台B 角度 (摄像头追踪人)
  float pan_B = 0.0f;       // 云台B Pan 角度 (°)
  float tilt_B = 0.0f;      // 云台B Tilt 角度 (°)
  // 灯云台A 角度 (灯光照射人)
  float pan_A = 0.0f;       // 灯云台 Pan 角度 (°)
  float tilt_A = 0.0f;      // 灯云台 Tilt 角度 (°)
  // 原始 HUSKYLENS 数据
  int xCenter = 0;
  int yCenter = 0;
  int width = 0;
  int height = 0;
  // height 自适应测距结果
  float distCam   = 0.0f;   // 摄像头到人距离(mm)
  float distLamp  = 0.0f;   // 灯到人距离(mm)
  float hPxRaw    = 0.0f;   // 原始 height 像素
  float hPxSmooth = 0.0f;   // 平滑后 height 像素
  // 绝对高度瞄准结果 (2.5.0)
  float faceHeightAbs  = 0.0f;  // 检测框中心绝对高度(mm, 离地)
  float chestHeightAbs = 0.0f;  // 胸口照射点绝对高度(mm, 离地)
  // 后滑轨位置
  int rearPosition = 0;
  // 检测有效
  bool valid = false;
};

// ===================== 灯节点注册信息 =====================
struct LampNode {
  String lampId;            // 灯节点唯一标识
  String zone;              // "left" / "center" / "right"
  int xAnchor = 0;          // 灯在前滑轨的安装位置 (mm)
  IPAddress ip;             // 灯节点 IP
  unsigned long lastPingMs = 0; // 上次心跳时间
  bool active = false;      // 是否活跃
  bool tracking = false;    // 是否正在追踪
};

// ===================== 外设 (extern) =====================
extern WebSocketsClient webSocket;
extern ESP8266WebServer server;
extern WiFiUDP udp;

// ===================== 运行状态 (extern) =====================
extern bool enableBroadcast;
extern bool enableAnnounce;
extern bool provisioningMode;
extern bool smartConfigActive;
extern bool smartConfigDoneHandled;
extern unsigned long smartConfigStartMs;
extern bool otaInProgress;
extern String firmwareChannel;
extern String otaStatus;
extern int otaProgress;
extern int lastOtaProgressLog;
extern int lastOtaProgressReport;
extern unsigned long lastOtaProgressReportMs;

extern unsigned long lastAnnounce;
extern unsigned long lastBroadcast;
extern unsigned long lastPing;

extern IPAddress cachedBroadcastIP;
extern bool broadcastIPCached;

// ===================== Nano 云台参数 (extern) =====================
extern int panDeg;
extern int tiltDeg;
extern int sliderMm;
extern int angleStep;
extern int sliderStep;
extern int panSpeedDeg;
extern int tiltSpeedDeg;
extern int sliderSpeedMm;

extern int rearSliderMm;
extern int rearStep;

// ===================== 设备配置 (extern) =====================
extern DeviceConfig cfg;
extern String deviceId;

// ===================== 分布式 PTZ 追踪状态 (extern) =====================
extern bool activeTracking;
extern PersonTrackResult lastTrackResult;
extern unsigned long lastPersonSeenMs;
extern unsigned long lastTrackPushMs;
extern LampNode activeLamp;
extern LampNode lampNodes[MAX_LAMP_NODES];
extern int lampNodeCount;

// 服务器后端下发的射灯 IP 列表
extern String lampIps[8];
extern int lampIpCount;
