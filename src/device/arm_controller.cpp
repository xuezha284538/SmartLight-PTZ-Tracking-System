#include "device/arm_controller.h"

// 当前速度档位
String currentArmSpeed = "normal";

// 摇杆连续运动状态
bool armJoystickActive = false;
float joystickX = 0.0f;
float joystickY = 0.0f;
float panVelocityDegPerSec = 0.0f;
float tiltVelocityDegPerSec = 0.0f;
unsigned long joystickExpireAt = 0;
unsigned long lastArmMotionUpdateAt = 0;
unsigned long lastNanoPositionSendAt = 0;

// 整数 → 字符串（静态缓冲，避免堆分配）
static const char* intToStr(int v) {
  static char buf[8];
  itoa(v, buf, 10);
  return buf;
}

void sendNano(char cmd, const String& value) {
  if (value.length() > 0) {
    nanoSerial.print(value);
  }
  nanoSerial.print('\n');

  delay(25);
  pollNano();

  DEBUG_SERIAL.print(F("[NANO] TX "));
  DEBUG_SERIAL.print(cmd);
  DEBUG_SERIAL.println(value);
}

void pollNano() {
  static char line[128];
  static uint8_t lineLen = 0;

  while (nanoSerial.available() > 0) {
    char ch = (char)nanoSerial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (lineLen > 0) {
        line[lineLen] = '\0';
        DEBUG_SERIAL.print(F("[NANO] RX "));
        DEBUG_SERIAL.println(line);
        lineLen = 0;
      }
      continue;
    }

    if (lineLen < sizeof(line) - 1) {
      line[lineLen++] = ch;
    } else {
      lineLen = 0;
      DEBUG_SERIAL.println(F("[NANO] RX line too long, dropped"));
    }
  }
}

void sendPanTilt() {
  panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
  tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
  sendNano('p', intToStr(panDeg));
  sendNano('t', intToStr(tiltDeg));
}

void sendSlider() {
  sliderMm = constrain(sliderMm, SLIDER_MIN, SLIDER_MAX);
  sendNano('x', intToStr(sliderMm));
}

void applyArmSpeed(const String& speed) {
  String normalized = speed;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized == "slow") {
    angleStep = 2;
    sliderStep = 5;
    panSpeedDeg = 4;
    tiltSpeedDeg = 3;
    sliderSpeedMm = 5;
  } else if (normalized == "fast") {
    angleStep = 5;
    sliderStep = 20;
    panSpeedDeg = 15;
    tiltSpeedDeg = 10;
    sliderSpeedMm = 30;
  } else {
    // normal
    angleStep = 5;
    sliderStep = 10;
    panSpeedDeg = 8;
    tiltSpeedDeg = 5;
    sliderSpeedMm = 10;
  }

  currentArmSpeed = normalized;

  sendNano('s', intToStr(panSpeedDeg));
  sendNano('S', intToStr(tiltSpeedDeg));
  sendNano('X', intToStr(sliderSpeedMm));
}

void getArmJoystickMaxSpeed(float& maxPanSpeed, float& maxTiltSpeed) {
  if (currentArmSpeed == "slow") {
    maxPanSpeed = 4.0f;
    maxTiltSpeed = 3.0f;
  } else if (currentArmSpeed == "fast") {
    maxPanSpeed = 15.0f;
    maxTiltSpeed = 10.0f;
  } else {
    // normal
    maxPanSpeed = 8.0f;
    maxTiltSpeed = 5.0f;
  }
}

void setArmJoystickMotion(float x, float y, int durationMs) {
  x = constrain(x, -1.0f, 1.0f);
  y = constrain(y, -1.0f, 1.0f);
  durationMs = constrain(durationMs, 100, 1000);

  float maxPanSpeed = 8.0f;
  float maxTiltSpeed = 5.0f;
  getArmJoystickMaxSpeed(maxPanSpeed, maxTiltSpeed);

  joystickX = x;
  joystickY = y;

  panVelocityDegPerSec = joystickX * maxPanSpeed;
  tiltVelocityDegPerSec = joystickY * maxTiltSpeed;

  armJoystickActive = true;
  joystickExpireAt = millis() + durationMs;
  lastArmMotionUpdateAt = millis();

  DEBUG_SERIAL.printf(
    "[ARM] joystick x=%.2f y=%.2f panVel=%.2f tiltVel=%.2f duration=%d\n",
    x,
    y,
    panVelocityDegPerSec,
    tiltVelocityDegPerSec,
    durationMs
  );
}

void stopArmJoystickMotion() {
  armJoystickActive = false;
  joystickX = 0.0f;
  joystickY = 0.0f;
  panVelocityDegPerSec = 0.0f;
  tiltVelocityDegPerSec = 0.0f;

  DEBUG_SERIAL.println("[ARM] joystick stopped");
}

void updateArmJoystickMotion() {
  if (!armJoystickActive) {
    return;
  }

  unsigned long now = millis();

  // 超时自动停止（前端断连/stop 丢包保护）
  if ((long)(now - joystickExpireAt) >= 0) {
    stopArmJoystickMotion();
    DEBUG_SERIAL.println("[ARM] joystick expired, auto stop");
    return;
  }

  if (lastArmMotionUpdateAt == 0) {
    lastArmMotionUpdateAt = now;
    return;
  }

  float dt = (now - lastArmMotionUpdateAt) / 1000.0f;
  lastArmMotionUpdateAt = now;

  // 防止帧间隔过大导致位置跳变
  if (dt <= 0.0f || dt > 0.2f) {
    return;
  }

  panDeg += (int)(panVelocityDegPerSec * dt);
  tiltDeg += (int)(tiltVelocityDegPerSec * dt);

  panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
  tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);

  // 每 80ms 向 Nano 发送一次当前位置
  if (now - lastNanoPositionSendAt >= 80) {
    sendNano('p', intToStr(panDeg));
    sendNano('t', intToStr(tiltDeg));
    lastNanoPositionSendAt = now;
  }
}

void handleArmAction(const String& action) {
  String normalizedAction = action;
  normalizedAction.trim();
  normalizedAction.toLowerCase();

  if (normalizedAction == "slider_position") {
    DEBUG_SERIAL.println(F("[ARM] slider_position ignored by lamp firmware"));
    return;
  }

  if (normalizedAction == "up") {
    tiltDeg += angleStep;
    tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
    sendNano('t', intToStr(tiltDeg));
  } else if (normalizedAction == "down") {
    tiltDeg -= angleStep;
    tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
    sendNano('t', intToStr(tiltDeg));
  } else if (normalizedAction == "left") {
    panDeg -= angleStep;
    panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
    sendNano('p', intToStr(panDeg));
  } else if (normalizedAction == "right") {
    panDeg += angleStep;
    panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
    sendNano('p', intToStr(panDeg));
  } else if (normalizedAction == "center") {
    panDeg = 0;
    tiltDeg = 0;
    sendPanTilt();
  } else if (normalizedAction == "home") {
    sendNano('A');
  } else if (normalizedAction == "stop") {
    DEBUG_SERIAL.println(F("[ARM] stop: keep current pan/tilt"));
    sendPanTilt();
  } else if (normalizedAction == "aim_person") {
    panDeg = 0;
    tiltDeg = -10;
    sendPanTilt();
  } else if (normalizedAction == "aim_cloth") {
    panDeg = 0;
    tiltDeg = 20;
    sendPanTilt();
  } else if (normalizedAction == "rear_left") {
    rearSliderMm -= rearStep;
    rearSliderMm = constrain(rearSliderMm, REAR_SLIDER_MIN, REAR_SLIDER_MAX);
    sendNano('r', intToStr(rearSliderMm));
  } else if (normalizedAction == "rear_right") {
    rearSliderMm += rearStep;
    rearSliderMm = constrain(rearSliderMm, REAR_SLIDER_MIN, REAR_SLIDER_MAX);
    sendNano('r', intToStr(rearSliderMm));
  } else if (normalizedAction == "rear_center") {
    rearSliderMm = CAM_HOME_REAR_SLIDER;   // 前滑轨回中 (Home 800mm, 与中区灯 xAnchor 对齐)
    sendNano('r', intToStr(rearSliderMm));
  } else {
    DEBUG_SERIAL.print(F("[ARM] unsupported lamp action: "));
    DEBUG_SERIAL.println(normalizedAction);
    return;
  }

  DEBUG_SERIAL.printf(
    "[ARM] action=%s pan=%d tilt=%d slider=%d rearSlider=%d angleStep=%d sliderStep=%d rearStep=%d\n",
    normalizedAction.c_str(),
    panDeg,
    tiltDeg,
    sliderMm,
    rearSliderMm,
    angleStep,
    sliderStep,
    rearStep
  );
}

void sendRearSlider() {
  rearSliderMm = constrain(rearSliderMm, REAR_SLIDER_MIN, REAR_SLIDER_MAX);
  sendNano('r', intToStr(rearSliderMm));
}

void applyRearSpeed(const String& speed) {
  String normalized = speed;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized == "slow") {
    rearStep = 5;
  } else if (normalized == "fast") {
    rearStep = 20;
  } else {
    rearStep = 10;
  }

  sendNano('R', intToStr(rearStep));
}
