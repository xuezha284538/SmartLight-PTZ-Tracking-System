#pragma once
#include "app_config.h"

static const int PAN_MIN = -90;
static const int PAN_MAX = 90;
static const int TILT_MIN = -45;
static const int TILT_MAX = 45;
static const int SLIDER_MIN = 0;
static const int SLIDER_MAX = 1200;
static const int REAR_SLIDER_MIN = 0;
static const int REAR_SLIDER_MAX = 1600;   // 前滑轨(视觉滑轨)行程 0~1600mm (2026-08 更新)

// 当前速度档位（applyArmSpeed 设置）
extern String currentArmSpeed;

// 摇杆连续运动状态
extern bool armJoystickActive;
extern unsigned long joystickExpireAt;
extern unsigned long lastArmMotionUpdateAt;

void sendNano(char cmd, const String& value = "");
void pollNano();
void sendPanTilt();
void sendSlider();
void applyArmSpeed(const String& speed);
void handleArmAction(const String& action);

// 摇杆连续运动接口
void getArmJoystickMaxSpeed(float& maxPanSpeed, float& maxTiltSpeed);
void setArmJoystickMotion(float x, float y, int durationMs);
void stopArmJoystickMotion();
void updateArmJoystickMotion();
void sendRearSlider();
void applyRearSpeed(const String& speed);
