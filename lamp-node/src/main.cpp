/**
 * 灯节点固件 — ESP8266 #2 / #3 / #4
 * 智慧服装店分布式 PTZ 追踪照明系统
 *
 * 职责:
 *   1. VL53L0X ToF 检测客户靠近 → HTTP 请求视觉节点追踪
 *   2. HTTP Server 接收视觉节点下发的 PTZ 数据 → 驱动灯云台
 *   3. BH1750 环境光自适应调光
 *   4. 双色温 LED (冷白/暖白 PWM)
 *   5. UDP 发现视觉节点 IP
 *
 * 硬件: ESP8266 (ESP-12E), BH1750, VL53L0X, 双色温 LED, 云台 (Arduino Nano)
 * 固件版本: 1.0.0 | 日期: 2026-07-04
 *
 * 与视觉节点(8266-master)对比:
 *   保留: WiFi, ToF, BH1750, LED PWM, Pan/Tilt 云台, HTTP
 *   移除: HUSKYLENS, WebSocket, OTA, 坐标转换, 视觉滑轨(前滑轨), 灯效, 呼吸定位
 */

// ==================== 头文件 ====================
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <WiFiUdp.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <BH1750.h>
#include <Adafruit_VL53L0X.h>

// ==================== 固件信息 ====================
#define FW_VERSION      "1.0.0"
#define FW_VERSION_CODE 10000

// ==================== 引脚定义 (与视觉节点相同) ====================
#define PIN_LED_COLD  D2   // GPIO4  冷白 LED PWM
#define PIN_LED_WARM  D1   // GPIO5  暖白 LED PWM
#define PIN_BLUR      D7   // GPIO13 散光/聚光控制
#define PIN_I2C_SDA   D5   // GPIO14 I2C 数据
#define PIN_I2C_SCL   D6   // GPIO12 I2C 时钟

// ==================== I2C 地址 ====================
#define BH1750_ADDR   0x23
#define VL53L0X_ADDR  0x29

// ==================== 常量 ====================
#define PAN_MIN         -90
#define PAN_MAX          90
#define TILT_MIN        -45
#define TILT_MAX         45
#define PWM_RANGE       1024
#define TEMP_MIN        2700   // 色温范围
#define TEMP_MAX        6500

#define TOF_TRIGGER_MM      2000    // ToF 触发距离 2m
#define TOF_DEBOUNCE_MS     1000    // 防抖时间 1s
#define TOF_LOST_DEBOUNCE_MS 800    // 离开防抖 0.8s
#define TOF_READ_INTERVAL_MS 50     // ToF 读取间隔
#define LIGHT_SMOOTH_MS      2000    // 灯光平滑过渡时间

#define PING_INTERVAL_MS     5000    // 心跳间隔
#define LUX_READ_INTERVAL_MS 2000    // 光照读取间隔
#define UDP_PORT             4210    // 视觉节点发现端口
#define UDP_DISCOVERY_TIMEOUT_MS 15000  // 发现超时后重试

// ===================== 配置文件名 =====================
const char* CONFIG_PATH = "/config.json";

// ===================== 默认值 =====================
const char* DEFAULT_SSID     = "NaHS";
const char* DEFAULT_PASSWORD = "123456789";

// ==================== 别名 ====================
// Serial = USB 串口 + Nano 通信 (共享 GPIO1/GPIO3, 57600 baud)
#define NANO_SERIAL  Serial
#define DEBUG_PRINT  Serial

// ==================== 全局对象 ====================
ESP8266WebServer httpServer(80);
BH1750 lightMeter;
Adafruit_VL53L0X tof;
WiFiUDP udp;

// ==================== 灯节点身份 ====================
String g_lampId   = "";
String g_zone     = "center";
int    g_xAnchor  = 800;        // 默认中区 (2026-08: 三区 400/800/1200)

// ==================== 视觉节点 ====================
IPAddress g_visionIP;
bool      g_visionDiscovered = false;
String    g_visionIPStr     = "";   // 从配置文件读取的手动 IP

// ==================== WiFi 配置 ====================
String g_ssid     = DEFAULT_SSID;
String g_password = DEFAULT_PASSWORD;
String g_staticIP = "";
String g_gateway  = "";
String g_subnetMask = "";

// ==================== 状态机 ====================
enum LampState {
  STATE_IDLE,
  STATE_TRACKING_START,  // 正在向视觉节点请求追踪
  STATE_TRACKING,        // 追踪中，接收 PTZ 数据
  STATE_TRACKING_STOP    // 正在通知视觉节点停止
};
LampState g_state = STATE_IDLE;

// ==================== 灯光参数 ====================
int  g_brightness           = 80;
int  g_colorTemp            = 4000;
bool g_autoMode             = true;
int  g_recommendedBrightness = 80;
int  g_recommendedTemp       = 4000;
int  g_luxAutoTarget         = 500;
int  g_luxAutoBrightness     = 50;

// ==================== 云台参数 ====================
int g_panDeg  = 0;
int g_tiltDeg = 0;

// ==================== 传感器状态 ====================
bool g_bh1750Ready = false;
bool g_tofReady    = false;

// ==================== ToF 计时 ====================
unsigned long g_lastTofReadMs       = 0;
unsigned long g_personDetectedAtMs  = 0;
unsigned long g_personLostAtMs      = 0;
bool          g_personPresent       = false;

// ==================== 灯光平滑过渡 ====================
unsigned long g_transitionStartMs   = 0;
int           g_transitionFromBr    = 80;
int           g_transitionFromTemp  = 4000;
int           g_transitionToBr      = 80;
int           g_transitionToTemp    = 4000;
bool          g_transitionActive    = false;

// ==================== 心跳 / 上报 ====================
unsigned long g_lastPingMs       = 0;
unsigned long g_lastLuxReadMs    = 0;

// ==================== 辅助函数 ====================

/** 整数→字符串 (静态缓冲复用，避免 String 堆分配) */
static const char* i2s(int v) {
  static char buf[12];
  itoa(v, buf, 10);
  return buf;
}

// ==================== Nano 通信 ====================

/**
 * 向 Arduino Nano 发送命令
 * 协议: "{value}\n" — 纯数值后跟换行符
 * sendPanTilt() 保证先发 pan 再发 tilt，Nano 依序解析
 */
void sendNanoRaw(const String& value) {
  if (value.length() > 0) {
    NANO_SERIAL.print(value);
  }
  NANO_SERIAL.print('\n');
}

/** 读取 Nano 回复 (非阻塞轮询) */
void pollNano() {
  static char line[128];
  static uint8_t lineLen = 0;

  while (NANO_SERIAL.available() > 0) {
    char ch = (char)NANO_SERIAL.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      if (lineLen > 0) {
        line[lineLen] = '\0';
        DEBUG_PRINT.print(F("[NANO] RX "));
        DEBUG_PRINT.println(line);
        lineLen = 0;
      }
      continue;
    }
    if (lineLen < (int)sizeof(line) - 1) {
      line[lineLen++] = ch;
    } else {
      lineLen = 0;
    }
  }
}

/** 发送 Pan/Tilt 角度给 Nano (先 Pan 再 Tilt) */
void sendPanTilt() {
  g_panDeg  = constrain(g_panDeg,  PAN_MIN,  PAN_MAX);
  g_tiltDeg = constrain(g_tiltDeg, TILT_MIN, TILT_MAX);

  sendNanoRaw(String(g_panDeg));
  delay(20);
  pollNano();

  sendNanoRaw(String(g_tiltDeg));
  delay(20);
  pollNano();

  DEBUG_PRINT.printf("[NANO] pan=%d tilt=%d\n", g_panDeg, g_tiltDeg);
}

/** 云台回正 */
void resetPTZ() {
  g_panDeg  = 0;
  g_tiltDeg = 0;
  sendPanTilt();
}

/** 设置云台移动速度 */
void setPTZSpeed(int panSpeedDps, int tiltSpeedDps) {
  sendNanoRaw(String(panSpeedDps));
  delay(15);
  pollNano();
  sendNanoRaw(String(tiltSpeedDps));
  delay(15);
  pollNano();
}

// ==================== LED 驱动 ====================

/**
 * 双色温 PWM 驱动
 * 色温 2700K(全暖白) ~ 6500K(全冷白) 线性映射
 */
void applyLight(int br, int tp) {
  tp = constrain(tp, TEMP_MIN, TEMP_MAX);
  br = constrain(br, 0, 100);

  int tempVal = map(tp, TEMP_MIN, TEMP_MAX, 0, PWM_RANGE);
  int briVal  = map(br, 0, 100, 0, PWM_RANGE);

  int pwmCold = (long)tempVal * briVal / PWM_RANGE;
  int pwmWarm = (long)(PWM_RANGE - tempVal) * briVal / PWM_RANGE;

  analogWrite(PIN_LED_COLD, PWM_RANGE - pwmCold);
  analogWrite(PIN_LED_WARM, PWM_RANGE - pwmWarm);
}

/** 启动灯光平滑过渡 (2s 内从当前值过渡到目标值) */
void startLightTransition(int toBr, int toTemp) {
  g_transitionFromBr   = g_brightness;
  g_transitionFromTemp = g_colorTemp;
  g_transitionToBr     = toBr;
  g_transitionToTemp   = toTemp;
  g_transitionStartMs  = millis();
  g_transitionActive   = true;
}

/** 推进灯光平滑过渡 (每帧在 loop() 中调用) */
void updateLightTransition() {
  if (!g_transitionActive) return;

  unsigned long elapsed = millis() - g_transitionStartMs;
  if (elapsed >= LIGHT_SMOOTH_MS) {
    // 过渡完成
    g_brightness = g_transitionToBr;
    g_colorTemp  = g_transitionToTemp;
    applyLight(g_brightness, g_colorTemp);
    g_transitionActive = false;
    return;
  }

  float ratio = (float)elapsed / LIGHT_SMOOTH_MS;
  int br = g_transitionFromBr + (int)((g_transitionToBr - g_transitionFromBr) * ratio);
  int tp = g_transitionFromTemp + (int)((g_transitionToTemp - g_transitionFromTemp) * ratio);
  applyLight(br, tp);
}

// ==================== 传感器 ====================

/** 读取光照度 (lux)，失败返回 -1 */
float readLux() {
  if (!g_bh1750Ready) return -1.0f;
  float lux = lightMeter.readLightLevel();
  return (lux < 0) ? -1.0f : lux;
}

/**
 * 读取 ToF 距离 (mm)
 * 返回 0 表示传感器未就绪或读取失败
 */
uint16_t readToF() {
  if (!g_tofReady) return 0;

  VL53L0X_RangingMeasurementData_t measure;
  tof.rangingTest(&measure, false);

  if (measure.RangeStatus != 4) {  // 4 = 超出量程
    return measure.RangeMilliMeter;
  }
  return 8190;  // 超量程 → 返回最大值
}

// ==================== 配置持久化 ====================

bool loadConfig() {
  // 先设默认值
  g_ssid       = DEFAULT_SSID;
  g_password   = DEFAULT_PASSWORD;
  g_staticIP   = "";
  g_gateway    = "";
  g_subnetMask = "";
  g_lampId     = "lamp-" + String(ESP.getChipId(), HEX);
  g_zone       = "center";
  g_xAnchor    = 800;
  g_visionIPStr = "";

  if (!LittleFS.exists(CONFIG_PATH)) {
    DEBUG_PRINT.println(F("[CONFIG] 无配置文件，使用默认值"));
    return false;
  }

  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f) return false;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    DEBUG_PRINT.printf("[CONFIG] JSON 解析失败: %s\n", err.c_str());
    return false;
  }

  g_ssid        = doc["ssid"]     | DEFAULT_SSID;
  g_password    = doc["password"] | DEFAULT_PASSWORD;
  g_staticIP    = doc["staticIP"] | "";
  g_gateway     = doc["gateway"]  | "";
  g_subnetMask  = doc["subnetMask"] | "";
  g_lampId      = doc["lampId"]   | g_lampId;
  g_zone        = doc["zone"]     | "center";
  g_xAnchor     = doc["xAnchor"]  | 800;
  g_visionIPStr = doc["visionIP"] | "";

  // 如果配置了手动 IP，解析它
  if (g_visionIPStr.length() > 0) {
    if (g_visionIP.fromString(g_visionIPStr)) {
      g_visionDiscovered = true;
      DEBUG_PRINT.printf("[CONFIG] 使用手动视觉节点 IP: %s\n", g_visionIP.toString().c_str());
    }
  }

  DEBUG_PRINT.printf("[CONFIG] lampId=%s zone=%s xAnchor=%d\n",
                     g_lampId.c_str(), g_zone.c_str(), g_xAnchor);
  return true;
}

bool saveConfig() {
  StaticJsonDocument<512> doc;
  doc["ssid"]     = g_ssid;
  doc["password"] = g_password;
  doc["staticIP"] = g_staticIP;
  doc["gateway"]  = g_gateway;
  doc["subnetMask"] = g_subnetMask;
  doc["lampId"]   = g_lampId;
  doc["zone"]     = g_zone;
  doc["xAnchor"]  = g_xAnchor;
  doc["visionIP"] = g_visionIPStr;

  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f) return false;
  if (serializeJson(doc, f) == 0) { f.close(); return false; }
  f.close();
  return true;
}

void clearConfig() {
  if (LittleFS.exists(CONFIG_PATH)) {
    LittleFS.remove(CONFIG_PATH);
  }
}

// ==================== WiFi 连接 ====================

bool connectWiFi(const String& ssid, const String& password,
                  const String& staticIP = "", const String& gateway = "", const String& subnetMask = "") {
  DEBUG_PRINT.printf("[WIFI] 连接 %s ...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);

  // 静态 IP：必须在 begin() 之前设置
  if (staticIP.length() > 0 && gateway.length() > 0 && subnetMask.length() > 0) {
    IPAddress ip, gw, mask;
    if (ip.fromString(staticIP) && gw.fromString(gateway) && mask.fromString(subnetMask)) {
      WiFi.config(ip, gw, mask);
      DEBUG_PRINT.printf("[WIFI] 静态 IP 已设置: %s\n", staticIP.c_str());
    } else {
      DEBUG_PRINT.println(F("[WIFI] 静态 IP 配置无效，回退 DHCP"));
    }
  }

  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < 15000) {
    delay(500);
    DEBUG_PRINT.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    DEBUG_PRINT.printf("\n[WIFI] 已连接 IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
  }

  DEBUG_PRINT.println(F("\n[WIFI] 连接超时"));
  return false;
}

// ==================== mDNS ====================

static bool mdnsStarted = false;

void beginMDNS() {
  if (mdnsStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (MDNS.begin(g_lampId.c_str())) {
    mdnsStarted = true;
    MDNS.addService("http", "tcp", 80);
    DEBUG_PRINT.printf("[mDNS] 已启动: %s.local\n", g_lampId.c_str());
  } else {
    DEBUG_PRINT.println(F("[mDNS] 启动失败"));
  }
}

void updateMDNS() {
  if (WiFi.status() != WL_CONNECTED) {
    if (mdnsStarted) {
      MDNS.end();
      mdnsStarted = false;
    }
    return;
  }
  if (!mdnsStarted) {
    beginMDNS();
    return;
  }
  MDNS.update();
}

// ==================== UDP 发现视觉节点 ====================

/**
 * 在 UDP 4210 端口监听视觉节点广播
 * 视觉节点每秒广播 JSON: {"deviceId":"LAMP-XXX","deviceType":"vision","ip":"..."}
 * 
 * 调用时机: loop() 中每次轮询
 */
void discoverVision() {
  if (g_visionDiscovered) return;  // 已发现，不再查找

  int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;

  char buf[256];
  int len = udp.read(buf, sizeof(buf) - 1);
  if (len <= 0) return;
  buf[len] = '\0';

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, buf)) return;

  const char* deviceType = doc["deviceType"] | "";
  if (strcmp(deviceType, "vision") != 0) return;  // 只看视觉节点

  IPAddress remoteIP = udp.remoteIP();
  g_visionIP = remoteIP;
  g_visionDiscovered = true;

  DEBUG_PRINT.printf("[UDP] 发现视觉节点: %s\n", g_visionIP.toString().c_str());
}

// ==================== HTTP 服务端 ====================

void addCors() {
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
}

void handleCorsPreflight() {
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  httpServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  httpServer.send(204);
}

/**
 * POST /lamp/control
 * 接收视觉节点下发的 PTZ 追踪数据
 * Body: {"tracking":true,"pan":-11.1,"tilt":-18.1,"brightness":80,"temp":4000,...}
 */
void handleLampControl() {
  addCors();

  if (!httpServer.hasArg("plain")) {
    httpServer.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  String body = httpServer.arg("plain");
  StaticJsonDocument<384> doc;
  if (deserializeJson(doc, body)) {
    httpServer.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  bool tracking = doc["tracking"] | false;

  if (!tracking) {
    // 视觉节点通知停止追踪
    DEBUG_PRINT.println(F("[HTTP] /lamp/control → 停止追踪"));
    if (g_state == STATE_TRACKING) {
      g_state = STATE_IDLE;
      resetPTZ();
    }
    httpServer.send(200, "application/json", "{\"result\":\"ok\"}");
    return;
  }

  // 追踪中: 应用 PTZ 数据
  float pan  = doc["pan"]  | (float)g_panDeg;
  float tilt = doc["tilt"] | (float)g_tiltDeg;
  int   br   = doc["brightness"] | g_brightness;
  int   temp = doc["temp"]        | g_colorTemp;

  // 范围校验: brightness 0-100, temp 2700-6500
  if (doc.containsKey("brightness") && (br < 0 || br > 100)) {
    String msg = "{\"error\":\"brightness out of range (0-100): " + String(br) + "\"}";
    httpServer.send(400, "application/json", msg);
    DEBUG_PRINT.printf("[HTTP] /lamp/control → brightness out of range: %d\n", br);
    return;
  }
  if (doc.containsKey("temp") && (temp < 2700 || temp > 6500)) {
    String msg = "{\"error\":\"temp out of range (2700-6500): " + String(temp) + "\"}";
    httpServer.send(400, "application/json", msg);
    DEBUG_PRINT.printf("[HTTP] /lamp/control → temp out of range: %d\n", temp);
    return;
  }

  // 更新状态
  g_state = STATE_TRACKING;

  // 2026-08-19 修改: 云台抖动修复 (死区 + 取整修正)
  // 1. (int) 截断改为 lround 四舍五入: 角度在整数边界徘徊时 (如 10.9/11.1) 不再产生 ±1° 跳变命令
  // 2. 整数角度无变化就不重发 Nano 命令: 旧逻辑每次收 HTTP 都调 sendPanTilt(),
  //    即使角度没变也重复发定位命令, 舵机反复重新咬合 → 肉眼可见的抖动。
  //    整数级比较等效 ≥1° 死区, 配合视觉端 cfg.ptzDeadZoneDeg (1.5°) 双重防抖。
  int newPan  = (int)lround(constrain(pan,  (float)PAN_MIN,  (float)PAN_MAX));
  int newTilt = (int)lround(constrain(tilt, (float)TILT_MIN, (float)TILT_MAX));
  if (newPan != g_panDeg || newTilt != g_tiltDeg) {
    g_panDeg  = newPan;
    g_tiltDeg = newTilt;
    sendPanTilt();
  } else {
    DEBUG_PRINT.printf("[HTTP] /lamp/control → PTZ unchanged (pan=%d tilt=%d), skip nano cmd\n",
                       g_panDeg, g_tiltDeg);
  }

  // 驱动灯光 (平滑过渡)
  if (g_brightness != br || g_colorTemp != temp) {
    startLightTransition(br, temp);
    DEBUG_PRINT.printf("[HTTP] /lamp/control → PTZ pan=%d tilt=%d br=%d temp=%d\n",
                       g_panDeg, g_tiltDeg, br, temp);
  }

  httpServer.send(200, "application/json", "{\"result\":\"ok\"}");
}

/** GET /status — 返回灯节点状态 */
void handleStatus() {
  addCors();

  StaticJsonDocument<256> doc;
  doc["lampId"]   = g_lampId;
  doc["zone"]     = g_zone;
  doc["xAnchor"]  = g_xAnchor;
  doc["state"]    = (g_state == STATE_TRACKING) ? "tracking" : "idle";
  doc["pan"]      = g_panDeg;
  doc["tilt"]     = g_tiltDeg;
  doc["brightness"] = g_brightness;
  doc["colorTemp"]  = g_colorTemp;
  doc["autoMode"]   = g_autoMode;
  doc["tofReady"]   = g_tofReady;
  doc["bh1750Ready"] = g_bh1750Ready;
  doc["visionIP"]   = g_visionDiscovered ? g_visionIP.toString() : "";
  doc["fwVersion"]  = FW_VERSION;
  doc["rssi"]       = WiFi.RSSI();

  String json;
  serializeJson(doc, json);
  httpServer.send(200, "application/json", json);
}

/** POST /setLight — 手动灯光控制 */
void handleSetLight() {
  addCors();

  if (!httpServer.hasArg("plain")) {
    httpServer.send(400, "application/json", "{\"error\":\"missing body\"}");
    return;
  }

  String body = httpServer.arg("plain");
  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) {
    httpServer.send(400, "application/json", "{\"error\":\"invalid json\"}");
    return;
  }

  if (doc.containsKey("brightness")) {
    g_brightness = doc["brightness"].as<int>();
  }
  if (doc.containsKey("colorTemp")) {
    g_colorTemp = doc["colorTemp"].as<int>();
  }
  if (doc.containsKey("autoMode")) {
    g_autoMode = doc["autoMode"].as<bool>();
  }
  if (doc.containsKey("recommendedBrightness")) {
    g_recommendedBrightness = doc["recommendedBrightness"].as<int>();
  }
  if (doc.containsKey("recommendedTemp")) {
    g_recommendedTemp = doc["recommendedTemp"].as<int>();
  }

  applyLight(g_brightness, g_colorTemp);

  DEBUG_PRINT.printf("[HTTP] /setLight br=%d temp=%d auto=%d\n",
                     g_brightness, g_colorTemp, g_autoMode);
  httpServer.send(200, "application/json", "{\"result\":\"ok\"}");
}

/** POST /resetWifi — 清除 WiFi 配置并重启 */
void handleResetWifi() {
  addCors();
  clearConfig();
  httpServer.send(200, "application/json", "{\"result\":\"config cleared, restarting\"}");
  delay(500);
  ESP.restart();
}

void setupHttpServer() {
  // CORS 预检
  httpServer.onNotFound([]() {
    if (httpServer.method() == HTTP_OPTIONS) {
      handleCorsPreflight();
    } else {
      httpServer.send(404);
    }
  });

  httpServer.on("/status",       HTTP_GET,  handleStatus);
  httpServer.on("/setLight",     HTTP_POST, handleSetLight);
  httpServer.on("/lamp/control", HTTP_POST, handleLampControl);
  httpServer.on("/resetWifi",    HTTP_POST, handleResetWifi);

  // OPTIONS for POST endpoints
  httpServer.on("/setLight",     HTTP_OPTIONS, handleCorsPreflight);
  httpServer.on("/lamp/control", HTTP_OPTIONS, handleCorsPreflight);
  httpServer.on("/resetWifi",    HTTP_OPTIONS, handleCorsPreflight);

  httpServer.begin();
  DEBUG_PRINT.println(F("[HTTP] 服务已启动 :80"));
}

// ==================== HTTP 客户端 (向视觉节点发送请求) ====================

/** POST /track/start — 通知视觉节点开始追踪 */
bool sendTrackStart() {
  if (!g_visionDiscovered) {
    DEBUG_PRINT.println(F("[HTTP] 视觉节点未发现，无法请求追踪"));
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, "http://" + g_visionIP.toString() + "/track/start");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(500);

  char json[128];
  snprintf(json, sizeof(json),
           "{\"zone\":\"%s\",\"lampId\":\"%s\",\"xAnchor\":%d}",
           g_zone.c_str(), g_lampId.c_str(), g_xAnchor);

  int code = http.POST((uint8_t*)json, strlen(json));
  if (code < 0) {
    DEBUG_PRINT.printf("[HTTP] /track/start → error: %s (timeout=%dms)\n",
                       http.errorToString(code).c_str(), 500);
  } else {
    DEBUG_PRINT.printf("[HTTP] /track/start → %d (zone=%s xAnchor=%d)\n",
                       code, g_zone.c_str(), g_xAnchor);
  }
  http.end();
  return (code == 200);
}

/** POST /track/stop — 通知视觉节点停止追踪 */
bool sendTrackStop() {
  if (!g_visionDiscovered) {
    DEBUG_PRINT.println(F("[HTTP] /track/stop → vision not discovered"));
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, "http://" + g_visionIP.toString() + "/track/stop");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(500);

  char json[96];
  snprintf(json, sizeof(json),
           "{\"zone\":\"%s\",\"lampId\":\"%s\"}",
           g_zone.c_str(), g_lampId.c_str());

  int code = http.POST((uint8_t*)json, strlen(json));
  if (code < 0) {
    DEBUG_PRINT.printf("[HTTP] /track/stop → error: %s (timeout=%dms)\n",
                       http.errorToString(code).c_str(), 500);
  } else {
    DEBUG_PRINT.printf("[HTTP] /track/stop → %d\n", code);
  }
  http.end();
  return (code == 200);
}

/** POST /ping — 心跳保活 */
bool sendPing() {
  if (!g_visionDiscovered) {
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, "http://" + g_visionIP.toString() + "/ping");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(200);

  char json[96];
  snprintf(json, sizeof(json),
           "{\"lampId\":\"%s\",\"zone\":\"%s\"}",
           g_lampId.c_str(), g_zone.c_str());

  int code = http.POST((uint8_t*)json, strlen(json));
  if (code < 0) {
    DEBUG_PRINT.printf("[HTTP] /ping → error: %s (timeout=%dms)\n",
                       http.errorToString(code).c_str(), 200);
  }
  http.end();
  return (code == 200);
}

// ==================== setup ====================

void setup() {
  // 串口初始化 (57600 baud — 与视觉节点 Nano 协议一致)
  Serial.begin(57600);
  delay(200);
  DEBUG_PRINT.println(F("\n\n=== 灯节点固件 v" FW_VERSION " ==="));
  DEBUG_PRINT.printf("[BOOT] ChipID=%X\n", ESP.getChipId());

  // GPIO 初始化
  pinMode(PIN_LED_COLD, OUTPUT);
  pinMode(PIN_LED_WARM, OUTPUT);
  pinMode(PIN_BLUR,     OUTPUT);
  digitalWrite(PIN_BLUR, LOW);
  analogWriteRange(PWM_RANGE);
  applyLight(0, 4000);  // 初始关灯

  // LittleFS
  if (!LittleFS.begin()) {
    DEBUG_PRINT.println(F("[FS] LittleFS 挂载失败!"));
  }

  // 加载配置
  loadConfig();

  // WiFi 连接
  if (!connectWiFi(g_ssid, g_password, g_staticIP, g_gateway, g_subnetMask)) {
    DEBUG_PRINT.println(F("[WIFI] 连接失败，将使用默认配置重试"));
  } else {
    beginMDNS();  // 启动 mDNS 主机名发现
  }

  // I2C 初始化
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(400000);

  // VL53L0X ToF 传感器
  if (tof.begin()) {
    g_tofReady = true;
  } else {
    DEBUG_PRINT.println(F("[TOF] VL53L0X 未检测到"));
    g_tofReady = false;
  }

  // BH1750 光照传感器
  if (lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE)) {
    g_bh1750Ready = true;
  } else {
    DEBUG_PRINT.println(F("[BH1750] 未检测到"));
    g_bh1750Ready = false;
  }

  // UDP 监听 (视觉节点发现)
  udp.begin(UDP_PORT);

  // HTTP 服务
  setupHttpServer();

  // Nano 云台初始化: 默认速度 + 回正
  setPTZSpeed(8, 5);
  resetPTZ();

  // 初始灯光
  applyLight(g_brightness, g_colorTemp);

  DEBUG_PRINT.printf("[READY] lampId=%s zone=%s xAnchor=%d ip=%s\n",
                     g_lampId.c_str(), g_zone.c_str(), g_xAnchor,
                     WiFi.localIP().toString().c_str());
}

// ==================== loop ====================

void loop() {
  // ---- mDNS 保活 ----
  updateMDNS();

  // ---- HTTP Server 轮询 ----
  httpServer.handleClient();

  // ---- UDP 发现视觉节点 ----
  discoverVision();

  // ---- 灯光平滑过渡推进 ----
  updateLightTransition();

  // ---- 传感器读取 & 状态机 ----
  unsigned long now = millis();

  // ToF 读取 (每 50ms)
  if (now - g_lastTofReadMs >= TOF_READ_INTERVAL_MS) {
    g_lastTofReadMs = now;
    uint16_t dist = readToF();

    if (g_tofReady && dist > 0 && dist < 8190) {
      bool nearby = (dist < TOF_TRIGGER_MM);

      // ─── 状态机 ───
      switch (g_state) {
        case STATE_IDLE:
          if (nearby) {
            if (g_personDetectedAtMs == 0) {
              g_personDetectedAtMs = now;
            } else if (now - g_personDetectedAtMs >= TOF_DEBOUNCE_MS) {
              // 防抖确认: 有人靠近 → 请求追踪
              DEBUG_PRINT.printf("[TOF] 有人靠近 %dmm，请求追踪\n", dist);
              if (sendTrackStart()) {
                g_state = STATE_TRACKING_START;
              }
              g_personDetectedAtMs = 0;
            }
          } else {
            g_personDetectedAtMs = 0;  // 重置防抖
          }
          break;

        case STATE_TRACKING_START:
          // 已发送追踪请求，等待视觉节点下发 PTZ
          // 此状态会在收到 /lamp/control 后转为 STATE_TRACKING
          if (!nearby) {
            g_personLostAtMs = (g_personLostAtMs == 0) ? now : g_personLostAtMs;
            if (now - g_personLostAtMs >= TOF_LOST_DEBOUNCE_MS) {
              DEBUG_PRINT.println(F("[TOF] 追踪请求阶段人已离开"));
              sendTrackStop();
              g_state = STATE_IDLE;
              g_personLostAtMs = 0;
            }
          } else {
            g_personLostAtMs = 0;
          }
          break;

        case STATE_TRACKING:
          if (!nearby) {
            if (g_personLostAtMs == 0) {
              g_personLostAtMs = now;
            } else if (now - g_personLostAtMs >= TOF_LOST_DEBOUNCE_MS) {
              // 人离开 > 0.8s → 停止追踪
              DEBUG_PRINT.println(F("[TOF] 人已离开，停止追踪"));
              sendTrackStop();
              resetPTZ();
              g_state = STATE_IDLE;
              g_personLostAtMs = 0;
            }
          } else {
            g_personLostAtMs = 0;
          }
          break;

        case STATE_TRACKING_STOP:
          // 暂不使用此状态
          g_state = STATE_IDLE;
          break;
      }
    }
  }

  // ---- 环境光自适应 (仅 IDLE 状态生效) ----
  if (g_autoMode && g_state != STATE_TRACKING && g_bh1750Ready) {
    if (now - g_lastLuxReadMs >= LUX_READ_INTERVAL_MS) {
      g_lastLuxReadMs = now;
      float lux = readLux();
      if (lux >= 0) {
        int error = g_luxAutoTarget - (int)lux;
        g_luxAutoBrightness += (int)(error * 0.1f);
        g_luxAutoBrightness = constrain(g_luxAutoBrightness, 5, 100);

        if (!g_transitionActive) {
          applyLight(g_luxAutoBrightness, g_colorTemp);
        }
      }
    }
  }

  // ---- 心跳 Ping 视觉节点 ----
  if (g_visionDiscovered && (now - g_lastPingMs >= PING_INTERVAL_MS)) {
    g_lastPingMs = now;
    sendPing();
  }

  // ---- Nano 串口轮询 ----
  pollNano();

  // ---- WiFi 保活 ----
  static unsigned long lastWifiCheck = 0;
  if (now - lastWifiCheck >= 10000) {
    lastWifiCheck = now;
    if (WiFi.status() != WL_CONNECTED) {
      DEBUG_PRINT.println(F("[WIFI] 断开，重连中..."));
      WiFi.reconnect();
    }
  }
}
