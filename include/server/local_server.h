#pragma once
#include "app_config.h"

void addCorsHeaders();
void addCorsHeadersWithMethods();
void setupDeviceHttpServer();
void handleStatus();
void handleSetLight();
void handleResumeBroadcast();
void handleStopBroadcast();
void handleStopAnnounce();
void handleResetWifi();
void handleLampIp();
