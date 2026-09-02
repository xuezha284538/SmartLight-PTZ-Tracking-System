#include "network/lamp_manager.h"
#include "network/lamp_notify.h"
#include "device/person_tracker.h"
#include "camera/roi_config.h"

// ===================== 灯节点注册 =====================
bool registerLamp(const String& lampId, const String& zone, int xAnchor, IPAddress ip) {
  // 检查是否已存在，更新信息
  for (int i = 0; i < lampNodeCount; i++) {
    if (lampNodes[i].lampId == lampId) {
      lampNodes[i].zone      = zone;
      lampNodes[i].xAnchor   = xAnchor;
      lampNodes[i].ip        = ip;
      lampNodes[i].lastPingMs = millis();
      lampNodes[i].active    = true;
      DEBUG_SERIAL.println("[LAMP] re-registered: " + lampId + " zone=" + zone + " ip=" + ip.toString());
      return true;
    }
  }

  // 复用已注销的空槽位
  for (int i = 0; i < lampNodeCount; i++) {
    if (!lampNodes[i].active) {
      lampNodes[i].lampId     = lampId;
      lampNodes[i].zone       = zone;
      lampNodes[i].xAnchor    = xAnchor;
      lampNodes[i].ip         = ip;
      lampNodes[i].lastPingMs = millis();
      lampNodes[i].active     = true;
      lampNodes[i].tracking   = false;
      DEBUG_SERIAL.println("[LAMP] registered (reused slot): " + lampId + " zone=" + zone +
                            " xAnchor=" + String(xAnchor) + " ip=" + ip.toString());
      return true;
    }
  }

  // 新增
  if (lampNodeCount >= MAX_LAMP_NODES) {
    DEBUG_SERIAL.println("[LAMP] max nodes reached, cannot register: " + lampId);
    return false;
  }

  LampNode& node = lampNodes[lampNodeCount];
  node.lampId     = lampId;
  node.zone       = zone;
  node.xAnchor    = xAnchor;
  node.ip         = ip;
  node.lastPingMs = millis();
  node.active     = true;
  node.tracking   = false;
  lampNodeCount++;

  DEBUG_SERIAL.println("[LAMP] registered: " + lampId + " zone=" + zone +
                        " xAnchor=" + String(xAnchor) + " ip=" + ip.toString() +
                        " total=" + String(lampNodeCount));
  return true;
}

// ===================== 灯节点注销 =====================
void unregisterLamp(const String& lampId) {
  for (int i = 0; i < lampNodeCount; i++) {
    if (lampNodes[i].lampId == lampId) {
      LampNode& node = lampNodes[i];

      // 如果正在追踪，先通知停止
      if (node.tracking) {
        notifyLampStop(node.ip);
        if (activeLamp.lampId == lampId) {
          activeLamp = LampNode();  // 清空
          activeTracking = false;
          resetTrackingDebounce();
        }
      }

      // swap-pop 回收槽位，避免数组永久填满
      lampNodes[i] = lampNodes[lampNodeCount - 1];
      lampNodeCount--;

      DEBUG_SERIAL.println("[LAMP] unregistered: " + lampId +
                            " (total=" + String(lampNodeCount) + ")");
      return;
    }
  }
}

// ===================== 心跳管理 =====================
bool updateLampPing(const String& lampId) {
  LampNode* node = findLampById(lampId);
  if (!node) return false;
  node->lastPingMs = millis();
  return true;
}

void checkLampHeartbeats() {
  unsigned long now = millis();
  for (int i = 0; i < lampNodeCount; i++) {
    if (!lampNodes[i].active) continue;

    if (now - lampNodes[i].lastPingMs > LAMP_HEARTBEAT_TIMEOUT_MS) {
      DEBUG_SERIAL.println("[LAMP] heartbeat timeout: " + lampNodes[i].lampId +
                            " (" + String((now - lampNodes[i].lastPingMs) / 1000) + "s)");

      if (lampNodes[i].tracking) {
        notifyLampStop(lampNodes[i].ip);
        if (activeLamp.lampId == lampNodes[i].lampId) {
          activeLamp = LampNode();
          activeTracking = false;
          resetTrackingDebounce();
          DEBUG_SERIAL.println("[LAMP] tracking stopped due to timeout");
        }
      }

      lampNodes[i].active = false;
    }
  }
}

// ===================== 查找 =====================
LampNode* findLampById(const String& lampId) {
  for (int i = 0; i < lampNodeCount; i++) {
    if (lampNodes[i].lampId == lampId && lampNodes[i].active) {
      return &lampNodes[i];
    }
  }
  return nullptr;
}

LampNode* findLampByZone(const String& zone) {
  for (int i = 0; i < lampNodeCount; i++) {
    if (lampNodes[i].zone == zone && lampNodes[i].active) {
      return &lampNodes[i];
    }
  }
  return nullptr;
}

LampNode* getActiveTrackingLamp() {
  if (activeLamp.lampId.length() == 0 || !activeLamp.active) return nullptr;
  return &activeLamp;
}

void setActiveTrackingLamp(const String& lampId) {
  LampNode* node = findLampById(lampId);
  if (node) {
    activeLamp = *node;
    activeLamp.tracking = true;
    node->tracking = true;
  }
}

// ===================== 状态查询 =====================
int getActiveLampCount() {
  int count = 0;
  for (int i = 0; i < lampNodeCount; i++) {
    if (lampNodes[i].active) count++;
  }
  return count;
}

bool isTrackingActive() {
  return activeTracking && activeLamp.active && activeLamp.tracking;
}

// ===================== 就近选灯 (2026-08 新增) =====================
// 候选来源:
//   1. 已注册灯节点 lampNodes[] (灯节点 POST /track/start 时带上 xAnchor)
//   2. 后端 /lamp-ip 下发的目标灯 (JSON 带可选 xAnchor 字段)
// 按人世界坐标 X 选最近灯, 滞回防抖; 切换时通知旧灯停止。
static String    s_selLampId;      // 当前选中灯 id (lampId 或 targetChipId)
static IPAddress s_selLampIp;      // 当前选中灯 IP
static int       s_selLampXAnchor; // 当前选中灯 xAnchor

// 扫描候选灯, 输出最近的 (id/ip/xAnchor/dist)
static void scanNearestCandidate(int personX, String& bestId, IPAddress& bestIp,
                                 int& bestXAnchor, int& bestDist) {
  bestDist = 0x7FFFFFFF;

  // 候选 1: 已注册灯节点
  for (int i = 0; i < lampNodeCount; i++) {
    if (!lampNodes[i].active) continue;
    int d = abs(lampNodes[i].xAnchor - personX);
    if (d < bestDist) {
      bestDist = d;
      bestId = lampNodes[i].lampId;
      bestIp = lampNodes[i].ip;
      bestXAnchor = lampNodes[i].xAnchor;
    }
  }

  // 候选 2: 后端下发目标 (带 xAnchor, 按 IP 去重)
  for (int i = 0; i < g_lampTargetCount; i++) {
    if (!g_lampTargets[i].valid || g_lampTargets[i].xAnchor < 0) continue;
    bool dup = false;
    for (int j = 0; j < lampNodeCount; j++) {
      if (lampNodes[j].active && lampNodes[j].ip == g_lampTargets[i].lampIp) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    int d = abs(g_lampTargets[i].xAnchor - personX);
    if (d < bestDist) {
      bestDist = d;
      bestId = g_lampTargets[i].targetChipId;
      bestIp = g_lampTargets[i].lampIp;
      bestXAnchor = g_lampTargets[i].xAnchor;
    }
  }
}

// 查找指定 id 的候选距离 (找不到返回 false)
static bool findCandidateDist(const String& id, int personX, int& dist) {
  for (int i = 0; i < lampNodeCount; i++) {
    if (lampNodes[i].active && lampNodes[i].lampId == id) {
      dist = abs(lampNodes[i].xAnchor - personX);
      return true;
    }
  }
  for (int i = 0; i < g_lampTargetCount; i++) {
    if (g_lampTargets[i].valid && g_lampTargets[i].xAnchor >= 0 &&
        g_lampTargets[i].targetChipId == id) {
      dist = abs(g_lampTargets[i].xAnchor - personX);
      return true;
    }
  }
  return false;
}

bool selectNearestLamp(int personX, IPAddress& outIp, int& outXAnchor, bool* switched) {
  if (switched) *switched = false;
  if (lampNodeCount == 0 && g_lampTargetCount == 0) return false;

  String bestId;
  IPAddress bestIp;
  int bestXAnchor = 0;
  int bestDist = 0;
  scanNearestCandidate(personX, bestId, bestIp, bestXAnchor, bestDist);

  if (bestId.length() == 0 || bestDist == 0x7FFFFFFF) return false;

  // 滞回: 已有选中灯时, 只有新灯明显更近才切换
  if (s_selLampId.length() > 0 && s_selLampId != bestId) {
    int curDist = 0;
    if (findCandidateDist(s_selLampId, personX, curDist) &&
        bestDist >= curDist - NEAREST_LAMP_HYSTERESIS_MM) {
      // 保持当前灯
      outIp = s_selLampIp;
      outXAnchor = s_selLampXAnchor;
      return true;
    }
  }

  // 切换到最近灯
  if (s_selLampId.length() > 0 && s_selLampId != bestId) {
    notifyLampStop(s_selLampIp);  // 旧灯停止追踪 (灯端回中/恢复默认)
    DEBUG_SERIAL.printf("[LAMP-SEL] 就近灯切换: %s -> %s (personX=%dmm)\n",
                        s_selLampId.c_str(), bestId.c_str(), personX);
    if (switched) *switched = true;
  } else if (s_selLampId.length() == 0) {
    DEBUG_SERIAL.printf("[LAMP-SEL] 就近灯选中: %s (xAnchor=%d, personX=%dmm)\n",
                        bestId.c_str(), bestXAnchor, personX);
    if (switched) *switched = true;
  }

  s_selLampId = bestId;
  s_selLampIp = bestIp;
  s_selLampXAnchor = bestXAnchor;

  outIp = bestIp;
  outXAnchor = bestXAnchor;
  return true;
}

void resetNearestLamp() {
  s_selLampId = "";
  s_selLampIp = IPAddress();
  s_selLampXAnchor = 0;
}

// ===================== 全局控制 =====================
void stopAllTracking() {
  for (int i = 0; i < lampNodeCount; i++) {
    if (lampNodes[i].tracking) {
      notifyLampStop(lampNodes[i].ip);
      lampNodes[i].tracking = false;
    }
  }
  activeLamp = LampNode();
  activeTracking = false;
  resetTrackingDebounce();
  resetNearestLamp();
  DEBUG_SERIAL.println("[LAMP] all tracking stopped");
}
