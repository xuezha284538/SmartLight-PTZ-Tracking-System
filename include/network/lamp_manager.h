#pragma once
#include "app_config.h"

// ===================== 灯节点注册 / 注销 =====================
// 由 track_server /track/start 调用
bool registerLamp(const String& lampId, const String& zone, int xAnchor, IPAddress ip);

// 由 track_server /track/stop 或心跳超时调用
void unregisterLamp(const String& lampId);

// ===================== 心跳管理 (track_server /ping 调用) =====================
bool updateLampPing(const String& lampId);

// 心跳超时检测 (§5.5: 15s 无心跳 → 离线 → 停止追踪)
// 由主循环每次调用
void checkLampHeartbeats();

// ===================== 查找 =====================
LampNode* findLampById(const String& lampId);
LampNode* findLampByZone(const String& zone);

// 当前正在追踪的灯节点
LampNode* getActiveTrackingLamp();
void setActiveTrackingLamp(const String& lampId);

// ===================== 状态查询 =====================
int getActiveLampCount();
bool isTrackingActive();

// ===================== 就近选灯 (2026-08 新增) =====================
// 按人的世界坐标 X (mm) 在已注册灯节点和带 xAnchor 的后端目标中选最近的一盏。
// 内置滞回 (NEAREST_LAMP_HYSTERESIS_MM): 新灯需明显更近才切换, 防止两区交界抖动;
// 切换时自动向旧灯发送 tracking:false。
// 返回 true 时输出选中灯的 ip 与 xAnchor; switched 指示本次是否发生了切换。
bool selectNearestLamp(int personX, IPAddress& outIp, int& outXAnchor, bool* switched);

// 重置就近选灯状态 (追踪停止/回中时调用, 下次检测重新选择)
void resetNearestLamp();

// ===================== 全局控制 =====================
void stopAllTracking();
