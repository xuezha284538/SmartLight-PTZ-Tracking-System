#include "server/local_server.h"
#include "config/config_manager.h"
#include "network/http_reporter.h"
#include "camera/roi_config.h"

void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
}

void addCorsHeadersWithMethods() {
  addCorsHeaders();
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

void handleStatus() {
  addCorsHeaders();
  server.send(200, "application/json", "{\"status\":\"已配网\"}");
}

void handleResumeBroadcast() {
  enableBroadcast = true;
  enableAnnounce = true;
  addCorsHeaders();
  server.send(200, "application/json", "{\"status\":\"resumed\"}");
  DEBUG_SERIAL.println("接收到网页指令：恢复广播");
}

void handleStopBroadcast() {
  enableBroadcast = false;
  addCorsHeaders();
  server.send(200, "application/json", "{\"result\":\"Broadcast stopped\"}");
  DEBUG_SERIAL.println("接收到网页指令：停止广播");
}

void handleStopAnnounce() {
  enableAnnounce = false;
  addCorsHeaders();
  server.send(200, "application/json", "{\"result\":\"Announce stopped\"}");
  DEBUG_SERIAL.println("接收到网页指令：停止上报");
}

void handleResetWifi() {
  clearConfig();
  addCorsHeaders();
  server.send(200, "application/json", "{\"result\":\"WiFi config cleared, restarting\"}");
  delay(800);
  ESP.restart();
}

// ===================== POST /lamp-ip =====================
// 后端配网完成后调用，传入射灯IP列表和结构化目标映射
// Body: {"lampIps":["192.168.1.21"], "targets":[{"targetIndex":1,"targetChipId":"LAMP-001","lampIp":"192.168.1.21"}]}
void handleLampIp() {
  addCorsHeadersWithMethods();

  if (server.method() == HTTP_OPTIONS) {
    server.send(204);
    return;
  }

  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  String body = server.arg("plain");
  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  // 1. 优先保存结构化目标映射 (targets[])
  JsonArray targets = doc["targets"].as<JsonArray>();
  if (!targets.isNull()) {
    setLampTargetsFromJson(targets);

    // 空数组时清空旧映射
    if (targets.size() == 0) {
      clearLampTargets();
      clearActiveTarget();
      DEBUG_SERIAL.println("[LAMP-IP] empty targets, cleared all mappings");
    }
  }

  // 2. lampIps[] 仅用于兼容旧版本, 不再用于全灯广播
  lampIpCount = 0;
  JsonArray ips = doc["lampIps"].as<JsonArray>();
  if (!ips.isNull()) {
    for (JsonVariant ip : ips) {
      if (lampIpCount >= 8) break;
      lampIps[lampIpCount++] = ip.as<String>();
    }
  }

  DEBUG_SERIAL.printf("[LAMP-IP] received: %d lamp IP(s), %d target(s)\n",
                      lampIpCount, g_lampTargetCount);
  for (int i = 0; i < g_lampTargetCount; i++) {
    DEBUG_SERIAL.printf("  target %d -> %s (%s)\n",
                        g_lampTargets[i].targetIndex,
                        g_lampTargets[i].targetChipId.c_str(),
                        g_lampTargets[i].lampIp.toString().c_str());
  }

  server.send(200, "application/json", "{\"ok\":true}");
}

void setupDeviceHttpServer() {
  server.onNotFound([]() {
    if (server.method() == HTTP_OPTIONS) {
      addCorsHeadersWithMethods();
      server.send(204);
    } else {
      server.send(404);
    }
  });

  server.on("/status", handleStatus);
  server.on("/stopBroadcast", handleStopBroadcast);
  server.on("/resumeBroadcast", handleResumeBroadcast);
  server.on("/stopAnnounce", handleStopAnnounce);
  server.on("/resetWifi", HTTP_POST, handleResetWifi);
  server.on("/lamp-ip", handleLampIp);

  server.begin();
}
