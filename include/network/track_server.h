#pragma once
#include "app_config.h"

// ===================== 注册视觉节点 HTTP 路由 =====================
// 扩展已有的 server 实例 (ESP8266WebServer, 端口 80)
void setupTrackServer();

// ===================== 路由处理器 =====================
// POST /track/start  — 灯节点请求开始追踪 (§5.2)
void handleTrackStart();

// POST /track/stop   — 灯节点请求停止追踪 (§5.3)
void handleTrackStop();

// POST /ping         — 灯节点心跳 (§5.5)
void handleTrackPing();

// GET /status        — 调试: 追踪状态 JSON (§5.6)
void handleTrackStatus();

// POST /config       — 更新追踪参数 (yPerson, dRails, deadZone, focalPx, ...) (§5.6)
void handleTrackConfig();

// POST /track/calibrate — 一步标定 focalPx (height 自适应测距)
void handleTrackCalibrate();
