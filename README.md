# 视界随光——基于视觉感知的智能美学追光系统

> 第十三届全国大学生物联网设计竞赛 全国一等奖

## 项目简介

为服装零售门店打造的智能照明闭环系统。顾客靠近展示区时，系统自动识别服装颜色、面料和穿搭结构，计算推荐亮度、色温、光束形态，并通过**二轴云台**调整照射方向，使重点光束始终对准顾客胸前的服装区域。

## 系统架构

```
┌─────────────────────────────────────────────────────────────────┐
│                         云端服务                                 │
│  Spring Boot + MySQL + Flask AI(SegFormer/ViT/MeanShift)        │
│  WebSocket 状态推送                                              │
└──────────────────────────┬──────────────────────────────────────┘
                           │ HTTP / WebSocket
    ┌──────────────────────┼──────────────────────┐
    │                      │                      │
    ▼                      ▼                      ▼
┌─────────┐         ┌─────────┐           ┌─────────┐
│ 左区灯   │         │ 中区灯   │           │ 右区灯   │
│ ESP8266  │         │ ESP8266  │           │ ESP8266  │
│ +Nano    │         │ +Nano    │           │ +Nano    │
│ +TMC2208 │         │ +TMC2208 │           │ +TMC2208 │
│ +云台A   │         │ +云台B   │           │ +云台C   │
│ +LED     │         │ +LED     │           │ +LED     │
│ +ToF     │         │ +ToF     │           │ +ToF     │
│ +BH1750  │         │ +BH1750  │           │ +BH1750  │
└────┬─────┘         └────┬─────┘           └────┬─────┘
     │                    │                      │
     └────────────────────┼──────────────────────┘
                          │ HTTP /lamp/control (100ms周期)
                          ▼
                   ┌──────────────┐
                   │  视觉节点     │
                   │  ESP8266     │
                   │  HUSKYLENS   │
                   │  ESP32-S3-CAM│
                   │  后滑轨电机  │
                   └──────────────┘
```

### 数据流

```
HUSKYLENS(人体框坐标)
  → 视觉节点ESP8266(坐标映射: 归一化→死区→atan角度→距离估算)
  → HTTP /lamp/control (100ms周期推送 pan/tilt 角度)
  → 灯节点ESP8266(lround取整 → 整数去重 → 串口转发)
  → Arduino Nano(角度→脉冲换算 → STEP/DIR生成)
  → TMC2208(微步驱动) → 42步进电机 → 云台转动
  → LED双色温PWM调光 + 电雾膜调光束形态
```

### 节点说明

| 节点 | 主控 | 核心外设 | 职责 |
|------|------|----------|------|
| 视觉节点 | ESP8266 | HUSKYLENS, ESP32-S3-CAM, 后滑轨电机 | 人体检测、服装拍照上传、坐标映射、角度计算、HTTP推送 |
| 左区灯节点 | ESP8266 + Nano | 二轴云台, 冷暖LED, ToF, BH1750 | 左区感知触发、照明执行 |
| 中区灯节点 | ESP8266 + Nano | 同上 | 中区感知触发、照明执行 |
| 右区灯节点 | ESP8266 + Nano | 同上 | 右区感知触发、照明执行 |

## 团队分工

| 成员 | 负责模块 |
|------|----------|
| **刘磊（我）** | **灯具二轴云台电机控制设计、ESP8266→Nano串口协议、Arduino Nano固件、TMC2208驱动配置、双重防抖机制、故障安全策略、三档速度控制与摇杆模式、PCB焊接与整机联调** |
| 队友A | HUSKYLENS人体检测读取、坐标映射算法（归一化→死区→atan角度换算→距离估算→胸口高度模型）、就近选灯逻辑、HTTP推送 |
| 队友B | Spring Boot服务端、MySQL、WebSocket状态推送 |
| 队友C | 服装AI识别（SegFormer-B2服装分割 + CIELAB-MeanShift主色提取 + ViT面料识别）、Flask AI服务 |
| 队友D | 前端Vue3 Web、Android App、微信小程序 |

---

## 我的负责部分：二轴云台电机控制系统

### 技术栈

ESP8266、Arduino Nano、TMC2208、42步进电机、STEP/DIR、串口通信

### 技术架构

- **ESP8266 + Arduino Nano 双层架构**：ESP8266负责网络通信（WiFi/HTTP/WebSocket），Nano专职生成高频STEP/DIR脉冲，通信与运动控制完全解耦
- **TMC2208 + 42步进电机**：1/16微步细分（0.1125°/步），STEP/DIR信号控制二轴云台Pan/Tilt运动
- **角度→脉冲数映射** + 三档速度控制 + 摇杆连续运动 + clip限幅防抖

### 任务描述

- 负责灯具二轴云台电机控制设计，实现Pan(-90°~+90°)/Tilt(-45°~+45°)角度调节，**1/16微步(0.1125°/步)定位精度优于1°**
- 设计ESP8266→Nano串口协议(57600 baud)，ESP8266接收上层角度后转发，Nano驱动TMC2208控制步进电机，**100ms周期响应<30ms**
- 编写Arduino Nano固件：角度→脉冲换算、STEP/DIR生成、微步配置、上电回零，**脉冲稳定性100%**
- 实现三档速度控制(4°/s~15°/s)与摇杆连续运动，支持手动方向/回正/定位，用于安装标定与现场演示
- 设计双重防抖：lround四舍五入+整数角度去重(≥1°死区)，**推送量降低80%**，消除云台抖动
- 实现clip限幅、目标丢失回零等故障安全策略
- 完成云台驱动电路、TMC2208驱动板接线及多路电源板焊接与整机联调调试
- 项目获第十三届全国大学生物联网设计竞赛全国一等奖

---

## 实现详解

### 1. ESP8266 + Nano 双层架构

**问题**：ESP8266同时运行Wi-Fi协议栈、HTTP服务、WebSocket连接，如果再让它生成高频STEP脉冲，网络中断会打断脉冲时序，导致电机丢步。

**解决**：将运动控制交给Arduino Nano，ESP8266只负责通信和业务逻辑。

```
ESP8266 (WiFi/HTTP/业务逻辑)
    │ Serial 57600 baud
    ▼
Arduino Nano (角度解析 → STEP/DIR脉冲生成)
    │ STEP/DIR
    ▼
TMC2208 (微步驱动) → 42步进电机 → 云台
```

Nano是纯运动控制器，没有网络开销，脉冲时序100%稳定。

相关代码：
- 视觉节点端：[`src/device/arm_controller.cpp`](src/device/arm_controller.cpp)
- 灯节点端：[`lamp-node/src/main.cpp`](lamp-node/src/main.cpp)

### 2. 串口通信协议

**协议格式**：`"{value}\n"`（纯数值 + 换行符）

**灯节点端**（先发Pan，再发Tilt）：

```cpp
void sendPanTilt() {
    g_panDeg = constrain(g_panDeg, PAN_MIN, PAN_MAX);   // -90~+90
    g_tiltDeg = constrain(g_tiltDeg, TILT_MIN, TILT_MAX); // -45~+45
    sendNanoRaw(String(g_panDeg));   // 先发Pan
    delay(20);
    pollNano();
    sendNanoRaw(String(g_tiltDeg));  // 再发Tilt
    delay(20);
    pollNano();
}
```

**视觉节点端**（支持更多命令）：

```cpp
void sendNano(char cmd, const String& value) {
    nanoSerial.print(value);
    nanoSerial.print('\n');
    delay(25);
    pollNano();
}

// 用法
sendNano('p', "90");    // Pan角度
sendNano('t', "-10");   // Tilt角度
sendNano('s', "8");     // Pan速度
sendNano('m', "16");    // 微步细分1/16
sendNano('A');          // 回零
```

### 3. TMC2208 配置与角度→脉冲映射

**TMC2208**：Trinamic静音步进驱动器，StealthChop静音模式，支持微步细分。

**配置**：1/16微步细分

```
步距角 = 1.8°（42电机标准）
单步分辨率 = 1.8° / 16 = 0.1125°/步
Pan全行程(-90°~+90°) = 180° / 0.1125° = 1600步
Tilt全行程(-45°~+45°) = 90° / 0.1125° = 800步
```

**Nano内部换算逻辑**：

```
1. 接收角度值（整数，如"45"）
2. 计算目标步数 = 角度 / 0.1125
3. 计算差值 = 目标步数 - 当前步数
4. 设置DIR方向（正/负）
5. 按速度频率生成STEP脉冲
6. 每个脉冲更新位置计数器
7. 到达目标位置停止
```

### 4. 三档速度控制

```cpp
void applyArmSpeed(const String& speed) {
    if (speed == "slow") {
        panSpeedDeg = 4;    // 4°/秒
        tiltSpeedDeg = 3;   // 3°/秒
    } else if (speed == "fast") {
        panSpeedDeg = 15;   // 15°/秒
        tiltSpeedDeg = 10;  // 10°/秒
    } else {  // normal
        panSpeedDeg = 8;    // 8°/秒
        tiltSpeedDeg = 5;   // 5°/秒
    }
    sendNano('s', intToStr(panSpeedDeg));
    sendNano('S', intToStr(tiltSpeedDeg));
}
```

Nano根据速度值设置STEP脉冲频率：`频率 = 速度(°/s) / 0.1125(°/步)`

| 档位 | Pan速度 | Tilt速度 | 脉冲频率 | 每步间隔 |
|------|---------|----------|----------|----------|
| slow | 4°/s | 3°/s | 35.6 Hz | 28ms |
| normal | 8°/s | 5°/s | 71.1 Hz | 14ms |
| fast | 15°/s | 10°/s | 133.3 Hz | 7.5ms |

### 5. 摇杆连续运动模式

用于安装标定和现场演示，前端摇杆控制云台连续运动。

```cpp
void setArmJoystickMotion(float x, float y, int durationMs) {
    // x,y ∈ [-1,1]，摇杆归一化坐标
    panVelocityDegPerSec = x * maxPanSpeed;
    tiltVelocityDegPerSec = y * maxTiltSpeed;
    armJoystickActive = true;
    joystickExpireAt = millis() + durationMs; // 超时自动停止
}

void updateArmJoystickMotion() {
    if (!armJoystickActive) return;
    // 超时保护
    if ((long)(millis() - joystickExpireAt) >= 0) {
        stopArmJoystickMotion();
        return;
    }
    // 速度积分→位置
    float dt = (millis() - lastArmMotionUpdateAt) / 1000.0f;
    if (dt <= 0.0f || dt > 0.2f) return; // 防帧间隔过大
    panDeg += (int)(panVelocityDegPerSec * dt);
    tiltDeg += (int)(tiltVelocityDegPerSec * dt);
    panDeg = constrain(panDeg, PAN_MIN, PAN_MAX);
    tiltDeg = constrain(tiltDeg, TILT_MIN, TILT_MAX);
    // 每80ms发送一次位置
    if (millis() - lastNanoPositionSendAt >= 80) {
        sendNano('p', intToStr(panDeg));
        sendNano('t', intToStr(tiltDeg));
        lastNanoPositionSendAt = millis();
    }
}
```

手动方向控制（按钮式，每次5°）：

```cpp
void handleArmAction(const String& action) {
    if (action == "up")         tiltDeg += angleStep;
    else if (action == "down")  tiltDeg -= angleStep;
    else if (action == "left")  panDeg -= angleStep;
    else if (action == "right") panDeg += angleStep;
    else if (action == "center") { panDeg = 0; tiltDeg = 0; }
    else if (action == "home")    sendNano('A');
    else if (action == "stop")    sendPanTilt();
}
```

### 6. 双重防抖机制

**问题**：HUSKYLENS检测框像素级波动→角度float值频繁变化（如10.1→10.3→10.7），每100ms推送→每次都驱动电机→微步反复咬合→肉眼可见抖动。

**解决**：三重防抖

**第一层：视觉端推送死区**（队友代码，我参与调试）

```cpp
// 角度变化小于1.5°不推送
bool ptzChanged = (fabs(result.pan_A - s_lastPanA) > cfg.ptzDeadZoneDeg ||
                   fabs(result.tilt_A - s_lastTiltA) > cfg.ptzDeadZoneDeg);
if (!ipChanged && !ptzChanged) return true; // 跳过
```

**第二层：灯端lround四舍五入**

```cpp
// 不用int截断（10.9→11.0边界会跳变），用lround更平滑
int newPan = (int)lround(constrain(pan, PAN_MIN, PAN_MAX));
int newTilt = (int)lround(constrain(tilt, TILT_MIN, TILT_MAX));
```

**第三层：灯端整数角度去重**

```cpp
if (newPan != g_panDeg || newTilt != g_tiltDeg) {
    g_panDeg = newPan;
    g_tiltDeg = newTilt;
    sendPanTilt();    // 只有变化才发Nano命令
} else {
    // skip - 不重发，避免电机反复咬合
}
```

**效果**：推送量降低约80%，云台抖动消除。

### 7. 故障安全策略

| 问题 | 检测方式 | 回退动作 |
|------|----------|----------|
| 云台角度越界 | constrain()限幅 | 保持最大安全角度 |
| 目标丢失 | ToF 0.8s无人 + HUSKYLENS 3s无框 | 停止追踪，云台回零 |
| 摇杆断连 | 超时保护 | 自动停止运动 |
| 灯节点心跳超时 | 15s无心跳 | 强制停止追踪 |
| 滑轨超限 | 限位开关 + 超时 | 停止电机，上报错误 |

```cpp
// 目标丢失回零
case STATE_TRACKING:
    if (!nearby) {
        if (now - g_personLostAtMs >= TOF_LOST_DEBOUNCE_MS) { // 0.8s
            sendTrackStop();
            resetPTZ();       // 云台回零
            g_state = STATE_IDLE;
        }
    }
    break;
```

### 8. 灯节点HTTP服务端

```cpp
// POST /lamp/control — 接收PTZ追踪数据
void handleLampControl() {
    String body = httpServer.arg("plain");
    StaticJsonDocument<384> doc;
    deserializeJson(doc, body);

    bool tracking = doc["tracking"];
    if (!tracking) {
        if (g_state == STATE_TRACKING) {
            g_state = STATE_IDLE;
            resetPTZ();  // 云台回零
        }
        return;
    }

    float pan = doc["pan"];
    float tilt = doc["tilt"];

    // 双重防抖
    int newPan = (int)lround(constrain(pan, -90, 90));
    int newTilt = (int)lround(constrain(tilt, -45, 45));

    if (newPan != g_panDeg || newTilt != g_tiltDeg) {
        g_panDeg = newPan;
        g_tiltDeg = newTilt;
        sendPanTilt();  // 只有变化才发Nano
    }
}
```

### 9. ToF触发追踪状态机

```
IDLE ──(ToF检测<2m, 防抖1s)──→ TRACKING_START
TRACKING_START ──(收到/lamp/control)──→ TRACKING
TRACKING ──(人离开, 防抖0.8s)──→ IDLE (云台回零)
```

### 10. LED双色温PWM驱动

冷白LED (GPIO4) + 暖白LED (GPIO5)，色温范围2700K~6500K。

```cpp
void applyLight(int br, int tp) {
    int tempVal = map(tp, 2700, 6500, 0, 1024);
    int briVal  = map(br, 0, 100, 0, 1024);
    int pwmCold = (long)tempVal * briVal / 1024;
    int pwmWarm = (long)(1024 - tempVal) * briVal / 1024;
    analogWrite(PIN_LED_COLD, 1024 - pwmCold); // 低电平有效
    analogWrite(PIN_LED_WARM, 1024 - pwmWarm);
}
```

灯光平滑过渡（2s线性插值），避免突然变化刺眼。

---

## 硬件设计

### 云台驱动电路

```
ESP8266 GPIO1(TX) → Arduino Nano RX
ESP8266 GPIO3(RX) ← Arduino Nano TX

Arduino Nano:
  D2 → TMC2208 STEP (Pan)
  D3 → TMC2208 DIR  (Pan)
  D4 → TMC2208 STEP (Tilt)
  D5 → TMC2208 DIR  (Tilt)
  D6 → TMC2208 EN   (使能)
```

### 多路电源板

| 输出 | 电压 | 芯片 | 供电对象 |
|------|------|------|----------|
| 路1 | 3.3V | LM2596 | ESP8266逻辑 |
| 路2 | 5V | LM2596 | Arduino Nano + 传感器 |
| 路3 | 12V | LM2596 | 步进电机 (TMC2208 VMOT) |
| 路4 | VCC_ADJ | LM2596 | LED驱动 |
| 路5 | 36V | LM2577(升压) | COB LED正向电压 |

保护：保险丝 + 肖特基续流二极管 + 去耦电容(0.1μF/100μF)

### I2C总线

```
ESP8266 D5(GPIO14) = SDA
ESP8266 D6(GPIO12) = SCL
Wire.begin(D5, D6), 400kHz

设备:
  BH1750    0x23  光照传感器
  VL53L0X   0x29  ToF测距
  HUSKYLENS 0x32  视觉传感器
```

---

## 关键参数

| 参数 | 值 |
|------|-----|
| 串口波特率 | 57600 baud |
| 控制周期 | 100ms (10Hz) |
| 步距角 | 1.8° |
| 微步细分 | 1/16 |
| 单步分辨率 | 0.1125°/步 |
| Pan范围 | -90° ~ +90° (1600步) |
| Tilt范围 | -45° ~ +45° (800步) |
| 速度档 | slow(4°/s) / normal(8°/s) / fast(15°/s) |
| 防抖死区 | 1.5°(推送端) + ≥1°(灯端) |
| 推送量降低 | ~80% |
| ToF触发距离 | 2000mm |
| 人丢失超时 | 3000ms |
| 灯心跳超时 | 15000ms |
| 灯光平滑过渡 | 2000ms |
| 色温范围 | 2700K ~ 6500K |

---

## 项目结构

```
SmartLight-PTZ-Tracking-System/
├── src/                        # 视觉节点固件 (ESP8266)
│   ├── main.cpp                # 主程序
│   ├── device/
│   │   ├── arm_controller.cpp  # 云台控制（摇杆/手动/速度）
│   │   ├── person_tracker.cpp # 人体追踪算法
│   │   ├── sensor_manager.cpp # 传感器管理
│   │   └── ota_manager.cpp    # OTA升级
│   ├── network/
│   │   ├── lamp_notify.cpp    # 灯节点HTTP推送
│   │   ├── lamp_manager.cpp   # 灯节点注册与选灯
│   │   ├── track_server.cpp    # 追踪HTTP服务端
│   │   ├── wifi_manager.cpp    # WiFi管理
│   │   ├── ws_client.cpp       # WebSocket客户端
│   │   ├── camera_upload.cpp  # 摄像头图像上传
│   │   ├── http_reporter.cpp  # HTTP上报
│   │   ├── mdns_manager.cpp   # mDNS服务发现
│   │   └── udp_discovery.cpp  # UDP设备发现
│   ├── server/
│   │   └── local_server.cpp   # 本地HTTP服务
│   ├── config/
│   │   └── config_manager.cpp # 配置管理
│   └── camera/
│       └── roi_config.cpp     # ROI配置
├── lamp-node/                  # 灯节点固件 (ESP8266)
│   ├── platformio.ini
│   └── src/
│       └── main.cpp            # 灯节点主程序（接收角度→驱动Nano→TMC2208）
├── include/                    # 头文件
├── lib/                        # 第三方库
│   └── HUSKYLENS/             # HUSKYLENS传感器库
├── docs/                       # 设计文档
│   ├── SYSTEM_DESIGN.md
│   ├── firmware_modules.md
│   └── 人像跟踪功能设计方案.md
├── platformio.ini             # PlatformIO配置
└── README.md
```

---

## 编译与烧录

### 环境要求

- PlatformIO Core (基于Python)
- ESP8266 Arduino框架
- Arduino Nano（作为运动控制器）

### 视觉节点编译

```bash
cd SmartLight-PTZ-Tracking-System
pio run -e esp12e          # 编译
pio run -t upload -e esp12e # 烧录
```

### 灯节点编译

```bash
cd lamp-node
pio run -e esp12e
pio run -t upload -e esp12e
```

### Arduino Nano固件

Nano固件独立编译烧录，接收串口角度值驱动TMC2208。使用Arduino IDE编译上传。

---

## 获奖情况

第十三届全国大学生物联网设计竞赛 **全国一等奖**

---

## License

本项目为竞赛参赛作品，仅供参考学习。
