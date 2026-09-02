#pragma once
#include "app_config.h"

bool connectWiFi(const String& ssid, const String& password, unsigned long timeoutMs,
                  const String& staticIP = "", const String& gateway = "", const String& subnetMask = "");
bool connectSavedWiFi();
void startParallelProvision();
void startAPPortal();
void handleProvisioningLoop();
bool ensureWiFiReady();
String getPortalHtml();
