#pragma once
#include "app_config.h"
#include "camera/roi_config.h"

void sendWsRegister();
void handleWsMessage(const String& text);
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length);
void beginWebSocketClient();

// WebSocket 推送 HUSKYLENS 人检测坐标
void sendPersonDetection(const PersonTrackResult& r);

// WebSocket 上报摄像头工作状态
void sendCamStatus(CamWorkStatus status, const String& message);

// WebSocket 上报追踪状态
void sendTrackingStatus(const char* status, float confidence, const String& message);

// WebSocket 上报 ROI 检测状态
void sendCamPresence(int personCount, float confidence);
