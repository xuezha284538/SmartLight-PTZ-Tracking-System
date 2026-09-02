#pragma once
#include "app_config.h"

const char* configPath();
String makeDeviceId();
bool   saveConfig(const DeviceConfig& c);
bool   loadConfig();
void   clearConfig();
void   ensureConfigDefaults(DeviceConfig& c);
