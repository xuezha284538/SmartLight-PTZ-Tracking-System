#pragma once
#include "app_config.h"

// ===================== 向灯节点推送 PTZ 数据 =====================
// POST /lamp/control (固定端口 80)
// lampIP: 灯节点 IP
// result: 追踪计算结果
bool notifyLamp(const IPAddress& lampIP, const PersonTrackResult& result);

// ===================== 通知灯节点停止追踪 =====================
bool notifyLampStop(const IPAddress& lampIP);

// ===================== 清除节流缓存 =====================
// 在切换活动灯或回中时调用, 确保下次推送不被节流跳过
void clearNotifyThrottleCache();

// ===================== 向所有已注册射灯推送 PTZ 坐标 =====================
// @deprecated 不再用于正常追踪路径, 正常追踪请使用 notifyLamp(activeLampIp, ...)
void notifyAllLamps(const PersonTrackResult& result);

// ===================== 通知所有已注册射灯停止追踪 =====================
// @deprecated 不再用于正常追踪路径, 正常停止请使用 notifyLampStop(activeLampIp)
void notifyAllLampsStop();
