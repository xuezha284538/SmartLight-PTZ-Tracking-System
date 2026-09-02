#include "device/sensor_manager.h"
#include "device/person_tracker.h"

void setupHardwareAndSensors() {
  Wire.begin(TOF_SDA_PIN, TOF_SCL_PIN);
  Wire.setClock(400000);

  // HUSKYLENS 初始化（仅一次，重复调用会触发 50×200ms=10s 重试阻塞）
  initPersonTracker();

  udp.begin(udpPort);
}
