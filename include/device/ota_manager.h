#pragma once
#include "app_config.h"

int  compareVersion(const String& a, const String& b);
void doOtaUpdate(const String& url, const String& version, int versionCode, const String& channel, const String& md5);
void doOtaUpdate(const String& url, const String& version);
