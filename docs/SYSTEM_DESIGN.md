# 智慧服装店分布式 PTZ 追踪照明系统 — 系统设计文档

> 版本: 2.5.1 | 日期: 2026-08-19
>
> 本文档按实际固件代码改写，反映 `8266-master/`、`lamp-node/`、`esp32-cam-firmware/` 三个工程的当前实现。
>
> 2.5.1 变更（高度实测复核修正）：
> - **离地高度修正**：复核实测，前滑轨（HUSKYLENS/摄像头）离地 **1600mm**（`camHeightMm` 默认值 1800→1600，固件 `app_config.h` / `config_manager.cpp` 同步修改），灯云台·后滑轨离地 **1800mm** = `camHeightMm + railHeightDiff`（高度差 200mm 不变）。§2.1 侧视图、§6.3 参数表、§6.4 数值示例已按新高度重算。
> - **已部署设备注意**：LittleFS 中已持久化 `camHeightMm=1800` 的设备不受代码默认值变化影响，需经 `/track/config` 或 WebSocket `person_track_config` 重下发 `camHeightMm=1600`（或删除 config.json 恢复默认）后生效。
>
> 2.5.0 变更（固件 FW 1.2.0）：
> - **射灯瞄准改为绝对高度模型（照胸口）**：删除固定角偏移 `TILT_BASE(-10°)` / `CHEST_OFFSET(-10°)`。旧模型 tilt 恒为 -20°，光斑落点随距离漂移（2m 处落在腰腹）；新模型由摄像头离地高度 + 人脸在画面中的位置反推**胸口绝对高度**，再按当前实测距离动态求 `tilt_B`/`tilt_A`，任意距离/身高下光斑都落在胸口。
> - **离地高度实测入库**：前滑轨（HUSKYLENS/摄像头）离地 **1800mm**（新参数 `camHeightMm`）；灯云台·后滑轨比前滑轨高 200mm，灯离地 **2000mm** = `camHeightMm + railHeightDiff`。
> - **人高 / yPerson 不再固定**：身高由人脸框中心高度按人体比例在线反推（眼位 ≈ 0.93×身高，胸口 ≈ 0.74×身高），`yPerson` 仅在检测框 height 无效时作降级深度备用。
> - **新增可配置参数**：`camHeightMm`（默认 1800，范围 800~3000）、`eyeHeightRatio`（默认 0.93，范围 0.85~0.98）、`chestHeightRatio`（默认 0.74，范围 0.60~0.85），支持 `/track/config`、WebSocket `person_track_config` 下发与 LittleFS 持久化；`/track/status` 新增 `faceHeightAbs`/`chestHeightAbs` 上报。
>
> 2.4.0 变更：
> - **前后滑轨高度差修正**：实际安装中前滑轨（视觉单元）比后滑轨（灯单元）**低 200mm**。`tilt_A` 不再直接等于 `tilt_B`，改为按高度差换算：`tilt_A = atan2(D_cam×tan(tilt_B) − railHeightDiff, D_lamp)`。新增可配置参数 `railHeightDiff`（默认 200mm，正值=灯高于摄像头，范围 -500~500mm），支持 `/track/config` 与 WebSocket `person_track_config` 下发。
>
> 2.3.0 变更：
> - **HUSKYLENS 自动触发**：由"物体追踪 + 二哈按键学习"改为**人脸识别模式自动检测**。上电时固件经 I2C `writeAlgorithm(ALGORITHM_FACE_RECOGNITION)` 切换算法；未学习人脸（ID=0）也输出检测框 → 识别到人即自动开始追踪，全程无需按键。`trackAlgorithm` 配置可切回物体追踪（1=按键学习旧行为）。
> - **就近选灯**：新增 `selectNearestLamp()`，按人的世界坐标 X 在已注册灯节点（或后端 /lamp-ip 带 xAnchor 的目标）中自动选最近一盏灯推送 PTZ，带 150mm 滞回防抖；切换时自动通知旧灯停止、前滑轨跟随到新灯 xAnchor。
> - **前滑轨行程更新**：0~1200mm → **0~1600mm**（`REAR_SLIDER_MAX`），回中位置 600mm → **800mm**（`CAM_HOME_REAR_SLIDER`，与中区灯 xAnchor 对齐）。
> - **灯安装位置更新**：灯A1/A2/A3 由 x=200/600/1000mm 改为 **x=400/800/1200mm**。
> - **测距常数按模式选择**：新增 `faceHeight`（默认 240mm，人脸框真实高度），人脸模式下 `D_cam = faceHeight × focalPx / h_px`；物体追踪模式仍用 `personHeight`。
>
> 2.2.0 变更：当前部署未安装云台B，HUSKYLENS 固定正对人；自动追踪循环移除 deadzone/方向确认/角度限幅三门控，检测到人即向灯节点推送 PTZ（灯端 `notifyLamp` 变化检测节流负责防抖）；`shouldSkipByDeadZone`/`confirmDirection`/`clampAngleStep` 及 `TRACK_CONFIRM_FRAMES`/`TRACK_ANGLE_LIMIT_PER_FRAME` 已从代码删除，`cfg.deadZone` 字段保留以兼容配置 API。

---

## 1. 系统概述

智慧服装店的分布式 AI 人体追踪照明系统。客户走近某服装展示区时，灯节点上的 ToF 传感器检测到有人，HTTP 通知视觉节点开始追踪；视觉节点驱动 HUSKYLENS 检测人体、通过 height 自适应算法估算距离、计算云台角度并下发给灯节点；同时通过 UART 触发 ESP32-S3-CAM 拍照上传后端做 YOLO 服装分析，后端再经 WebSocket 回传颜色/材质参数自适应调整灯光。

### 核心流程

1. 客户走近展示区 → 灯节点 ToF 检测到有人（2m 内，1s 防抖）
2. 灯节点 HTTP POST `/track/start` 通知视觉节点
3. 视觉节点驱动前滑轨（视觉单元）移动到对应 xAnchor，HUSKYLENS 开始人体检测
4. 视觉节点经 UART 向 ESP32-S3-CAM 发送 `Tstart` + `C`，拍第一张照片上传后端
5. HUSKYLENS（人脸识别模式）自动输出人脸检测框 → 视觉节点坐标转换（height 自适应测距 + atan2 精确角度）
6. 视觉节点按人位置就近选灯，计算灯云台A 的 Pan/Tilt 并 HTTP 下发给该灯节点（当前无云台B，HUSKYLENS 固定正对人）
7. 灯节点驱动灯云台转动 → 灯光照射人物胸口
8. 追踪中每 3 秒 ESP32-S3-CAM 拍照一张，后端持续分析
9. 人离开（HUSKYLENS 3s 无人）→ 停止追踪 → 前滑轨回中 → 灯恢复默认 → ESP32-S3-CAM 拍最后一张

> 2.3.0 起 HUSKYLENS 由固件自动切换为人脸识别模式：识别到人（含未学习人脸 ID=0）即自动触发，无需在二哈上按键学习目标；`trackAlgorithm=1` 可切回物体追踪（按键学习，wiki §7.2 旧行为）。追踪中人跨区移动时，视觉节点自动切换到就近的灯（150mm 滞回），并驱动前滑轨跟随到该灯 xAnchor。

---

## 2. 物理布局

> 2026-07-21 物理布局已对调：视觉单元位于前滑轨（靠墙侧，可滑动），灯单元位于后滑轨（靠客户侧，固定）。代码中变量名 `rear*` 为历史命名保留，语义指"视觉单元所在滑轨"。
>
> 2026-08-18 高度实测（2026-08-19 复核修正）：两条滑轨不在同一高度——**前滑轨（视觉单元）比后滑轨（灯单元）低 200mm**（`railHeightDiff`）。绝对高度：**HUSKYLENS·前滑轨（低）离地 1600mm**（`camHeightMm`），**灯云台·后滑轨（高 200mm）离地 1800mm** = `camHeightMm + railHeightDiff`。俯视图不体现高度，垂直方向几何见 §2.1 侧视图与 §6.4 Step 6/7 的绝对高度瞄准。
>
> 人的身高与站立深度（`yPerson`）均不固定：距离由人脸框像素高度在线测得，胸口照射点高度由人脸绝对高度按人体比例反推（§6.4 Step 6），不依赖任何固定人高假设。

### 2.1 俯视图

```
                              WALL(展示墙/服装挂架)
        ┌──────────┬──────────┬──────────┐
        │  服装A   │  服装B   │  服装C   │
        │  (左区)  │  (中区)  │  (右区)  │
        └──────────┴──────────┴──────────┘

前滑轨 (可水平滑动 0~1600mm, 靠墙侧) — 视觉单元
 ═══ 📷HUSKYLENS  📸ESP32-S3-CAM ═══════════
       │              │
       人体检测       拍照上传
       (ESP8266 #1 视觉节点 - 全部控制, HUSKYLENS 固定正对人)

        ← D_rails = 200mm (前后滑轨间距) →

后滑轨 (固定不滑动, 靠客户侧) — 灯单元
 ═══ 🔦灯A1     🔦灯A2     🔦灯A3 ═══════════════
       ●           ●           ●
    x=400mm    x=800mm    x=1200mm
    云台A1      云台A2      云台A3
    (ESP#2)    (ESP#3)     (ESP#4)

        ← Y_person = 2000mm (客户站立深度，可配置) →

                   🧍 客户
```

视觉深度 = `yPerson + dRails`（靠墙更深），灯深度 = `yPerson`（靠客户更近）。

**侧视图**（高度 × 深度平面，2026-08-18 实测绝对高度）：

```
   高度 ↑
  1800mm┤                     🔦 灯单元 (后滑轨, 高)  ← 灯离地 1800mm
        │                     ═══════════════════════
        │        ┌── 200mm ──┘  railHeightDiff (灯高于摄像头)
  1600mm┤    📷 视觉单元 (前滑轨, 低)  ← HUSKYLENS 离地 1600mm
        │    ═══════════════
        │                          ╲ tilt_A (动态, 瞄准胸口)
        │                           ╲  光斑落点 = 胸口 ≈ 0.74×身高
     0mm└───────────────────────────────────────────────🧍────→ 深度 (远离展示墙)
            墙侧    ← dRails=200mm →   客户侧   ← yPerson(不固定) →   客户
```

前滑轨比后滑轨低 200mm：摄像头视线求得的胸口点换算到灯基准时要再减去这段高度差，灯云台需额外下俯（见 §6.4 Step 7）。人高与站位深度不固定，胸口高度按人脸实测高度 × (`chestHeightRatio`/`eyeHeightRatio`) 在线反推，射灯光斑始终瞄准胸口而非固定角度落点。

### 2.2 前滑轨托盘俯视 (视觉单元)

HUSKYLENS、ESP32-S3-CAM 固定在同一托盘上，正对客户方向。当前部署未安装云台B，视觉单元不主动转向；`pan_B`/`tilt_B` 仍由算法计算，用于状态上报与 `tilt_A` 派生。

> 2.3.0 起 HUSKYLENS 由固件在初始化时自动切换为**人脸识别**模式（`writeAlgorithm(ALGORITHM_FACE_RECOGNITION)`），自动检测画面中的人（未学习人脸 ID=0 同样输出检测框）。若二哈固件较老不支持该 I2C 命令，串口会打印 `writeAlgorithm 失败`，需在二哈菜单手动切换到人脸识别。

---

## 3. 组件清单

| 组件 | 数量 | 安装位置 | 功能 |
|------|------|---------|------|
| ESP8266 (ESP-12E) | 4 | 见 §3.1 | 控制与 WiFi 通信 |
| ESP32-S3-WROOM-1U + OV3660 | 1 | 前滑轨托盘 | 拍照上传 (XGA 1024x768 JPEG) |
| Arduino Nano | 4 | 每个 ESP8266 一个 | 舵机/步进电机驱动 |
| DFRobot HUSKYLENS | 1 | 前滑轨托盘 | AI 人体检测 + 检测框坐标输出 |
| 云台B (Pan/Tilt) | 0 | 前滑轨托盘 | 当前部署未安装；HUSKYLENS 固定正对人，`pan_B`/`tilt_B` 仍计算用于状态上报与 `tilt_A` 派生 |
| 前滑轨电机 (视觉滑轨) | 1 | 前滑轨 | 驱动托盘在 xAnchor 之间移动 |
| 灯云台A (Pan/Tilt) | 3 | 后滑轨 x=400/800/1200mm | 让灯始终照向人 |
| VL53L0X ToF 传感器 | 3 | 每个灯云台上 | 检测客户靠近（2m 范围） |
| BH1750 光照传感器 | 3 | 每个灯云台上 | 环境光自适应调光 |
| 双色温 LED 灯 | 3 | 每个灯云台上 | 冷白+暖白 PWM (2700-6500K) |

> 视觉节点固件（`8266-master/`）保留了 ToF + LED PWM + BLUR 引脚定义（`app_config.h` D1/D2/D7），源自统一固件历史。当前架构中视觉节点的主职责是追踪，ToF/LED 代码路径仍可运行但不是系统核心。

### 3.1 ESP8266 分工

| # | 名称 | 位置 | 外设 | 固件 |
|---|------|------|------|------|
| #1 | 视觉节点 | 前滑轨 (视觉滑轨) | HUSKYLENS(I2C 0x32), VL53L0X(I2C 0x29), 前滑轨电机, ESP32-S3-CAM(UART) | `8266-master/` |
| #2 | 左区灯节点 | 后滑轨 x=400mm | 灯A1, 云台A1, ToF(0x29), BH1750(0x23) | `lamp-node/` |
| #3 | 中区灯节点 | 后滑轨 x=800mm | 灯A2, 云台A2, ToF(0x29), BH1750(0x23) | `lamp-node/` |
| #4 | 右区灯节点 | 后滑轨 x=1200mm | 灯A3, 云台A3, ToF(0x29), BH1750(0x23) | `lamp-node/` |

### 3.2 硬件连线

#### 3.2.1 I2C 总线（视觉节点）

视觉节点的 I2C 总线挂两个从机，ESP8266 作主机：

```
  ESP8266 #1 (Master)              I2C 总线 (400kHz)
    D5=SDA (GPIO14) ─────────────── SDA ───── HUSKYLENS SDA  (0x32)
    D6=SCL (GPIO12) ─────────────── SCL ───── HUSKYLENS SCL  (0x32)
                                   │
                                   ├─────── VL53L0X SDA      (0x29)
                                   └─────── VL53L0X SCL      (0x29)

  上拉: SDA/SCL 各接 4.7kΩ 到 3.3V (HUSKYLENS 模块自带)
```

| 设备 | I2C 地址 | 角色 | 引脚 |
|------|---------|------|------|
| ESP8266 #1 | — | Master | D5=SDA, D6=SCL |
| HUSKYLENS | 0x32 | Slave | SDA/SCL |
| VL53L0X | 0x29 | Slave | SDA/SCL |

灯节点的 I2C 总线挂 VL53L0X(0x29) + BH1750(0x23) 两个从机，引脚同为 D5/D6。

> D6 对应 GPIO12（MTDI），是 ESP8266 的 boot-strapping 引脚。I2C 上拉会将其拉高，存在启动失败风险。如遇偶发启动失败，可将 I2C 改到 D1/D2（GPIO5/GPIO4）。

#### 3.2.2 UART 连接（视觉节点 ↔ ESP32-S3-CAM）

ESP32-S3-CAM **不接入 I2C 总线**，通过 UART 与视觉节点通信：

```
  ESP8266 #1                          ESP32-S3-CAM
  Serial1 (GPIO2, 仅 TX) ──────────── UART2 RX (GPIO7)
                                      UART2 TX (GPIO8) → (未接, 视觉节点不收)

  波特率: 115200
  协议: 文本行命令 (见 §8)
```

> ESP8266 的 Serial1 只有 TX（GPIO2），视觉节点只发不收。ESP32-S3 的状态查询回复（`R` 命令）虽在固件中实现，但视觉节点不读取。

#### 3.2.3 ESP32-S3-CAM 内部连接（OV3660 DVP + SCCB）

OV3660 通过 DVP 并行总线和 SCCB 连接到 ESP32-S3，是开发板上的板上硬连接：

| OV3660 引脚 | 接 ESP32-S3 GPIO | 功能 |
|------------|-----------------|------|
| SIOD (SDA) | GPIO4 | SCCB 数据 (esp_camera 库自动管理) |
| SIOC (SCL) | GPIO5 | SCCB 时钟 |
| XCLK | GPIO15 | 20MHz 像素时钟输入 |
| PCLK | GPIO17 | 像素时钟输出 |
| VSYNC | GPIO9 | 帧同步 |
| HREF | GPIO18 | 行同步 |
| D0~D7 | GPIO10/11/12/13/14/21/47/48 | 8 位并行数据 |
| PWDN | GND | 低电平使能 |

> OV3660 的 SCCB 地址为 0x3C（区别于 OV2640 的 0x60），`esp_camera` 库在初始化时自动探测传感器型号并适配。源码头部注释（`main.cpp` / `platformio.ini`）仍标注"OV2640"为历史残留，实际引脚配置按 OV3660 接线，与 `esp32-cam-firmware/HARDWARE_NOTES.md` 的 OV3660 接线表一致。
>
> ESP32-S3-WROOM-1U 必须使用带 PSRAM 的模组（推荐 N8R8，8MB Flash + 8MB Octal PSRAM）。GPIO26~32 为 SPI Flash/PSRAM 专用，GPIO33~37 在 Octal PSRAM 模式下被占用，均不可用。

---

## 4. 通信架构

### 4.1 全局网络拓扑

```
                          ┌──────────────┐
                          │  WiFi 路由器   │
                          │  192.168.1.x  │
                          └───┬──┬──┬──┬──┘
                              │  │  │  │
        ┌─────────────────────┼──┼──┼──┼─────────────────┐
        │                     │  │  │  │                  │
   ┌────▼────┐    ┌────▼──┐ ┌▼──▼──▼──┐    ┌─────────────▼──┐
   │ESP8266#1│    │ESP32  │ │ESP8266  │    │ camera-server  │
   │视觉节点  │◄───│S3-CAM │ │#2,#3,#4│    │ Spring Boot    │
   │         │UART│拍照上传│ │灯节点   │    │ :80            │
   │HTTP :80 │    └───┬───┘ └────┬────┘    │ /api/camera/*  │
   │WS :80   │       │HTTP       │HTTP     │ /ws/device     │
   └─────────┘       │           │         └────────────────┘
        │            │           │
        │ UDP :4210  │           │
        └────────────┼───────────┘
             广播发现  │  POST上传
                      ▼
              ┌──────────────┐
              │ camera-server│
              └──────────────┘
```

### 4.2 通信协议总览

| 方向 | 协议 | 端口/引脚 | 频率 | 内容 |
|------|------|----------|------|------|
| 视觉节点 → 灯节点 | HTTP POST `/lamp/control` | 80 | 每 100ms | `{tracking, pan, tilt, brightness, temp, personData}` |
| 灯节点 → 视觉节点 | HTTP POST `/track/start` | 80 | 事件触发 | `{zone, lampId, xAnchor}` |
| 灯节点 → 视觉节点 | HTTP POST `/track/stop` | 80 | 事件触发 | `{zone, lampId}` |
| 灯节点 → 视觉节点 | HTTP POST `/ping` | 80 | 每 5s | `{lampId, zone}` |
| 视觉节点 → ESP32-S3-CAM | UART (Serial1→UART2, 115200) | GPIO2→GPIO7 | 命令触发 | 文本行命令: `C`/`Z`/`T`/`S` |
| ESP32-S3-CAM → 后端 | HTTP multipart POST `/api/camera/upload` | 80 | 命令触发 | JPEG + `{deviceId, zone, trigger, timestamp}` |
| 视觉节点 ↔ 后端 | WebSocket `/ws/device` | 80 | 双向 | 见 §9.3 |
| 视觉节点 → 局域网 | UDP 广播 | 4210 | 每 5s | 设备发现 `{"type":"announce","device":"cam",...}` |
| 视觉节点 → 灯节点 | Nano 串口 (共享 Serial) | 57600 | 命令触发 | 数值+换行 (pan/tilt/slider) |

---

## 5. 工作流程

### 5.1 完整交互时序

```
#2(灯节点)     #1(视觉节点)      ESP32-S3-CAM    HUSKYLENS     后端Server
   │                │                │            │              │
   │ ToF检测到人     │                │            │              │
   │─/track/start──→│                │            │              │
   │ {zone,xAnchor} │                │            │              │
   │                │ 前滑轨→xAnchor  │            │              │
   │                │──Zleft────────→│(UART)      │              │
   │                │──Tstart───────→│(UART)      │              │
   │                │──C────────────→│(UART)      │              │
   │                │                │ 拍照(全景)  │              │
   │                │                │──upload────┼───────────→  │
   │                │                │            │   YOLO分析   │
   │                │                │            │   颜色/材质   │
   │                │←──analysis_result(WS)─────────────────────  │
   │                │ 更新灯光参数    │            │              │
   │                │──request()────┼───────────→│              │
   │                │←─ xCenter,yCenter,width,height ────────────│
   │                │                │            │              │
   │                │ height自适应测距+atan2坐标转换 │            │
   │←/lamp/control──│                │            │              │
   │ {pan_A,tilt_A} │                │            │              │
   │ 驱动灯云台      │                │            │              │
   │                │                │            │              │
   │   ═══════ 追踪中 (循环) ═══════              │              │
   │                │                │            │              │
   │                │──Ttrack───────→│(UART,每3s)  │              │
   │                │──C────────────→│(UART)      │              │
   │                │                │ 拍照(场景)  │              │
   │                │                │──upload────┼───────────→  │
   │                │                │            │              │
   │   ═══════ 人离开 ═══════                   │              │
   │                │                │            │              │
   │                │ HUSKYLENS 3s无人│           │              │
   │                │──Tstop────────→│(UART)      │              │
   │                │──C────────────→│(UART)      │              │
   │                │──S────────────→│(UART)      │              │
   │                │                │ 最后一张     │              │
   │                │                │──upload────┼───────────→  │
   │                │ 前滑轨回中(800mm)│           │              │
   │←tracking:false─│                │            │              │
   │ 灯恢复默认      │                │            │              │
```

> 2.3.0 起追踪过程中视觉节点每帧按人位置就近选灯（150mm 滞回防抖），人跨区移动时自动切换：旧灯收到 `tracking:false` 恢复默认，前滑轨跟随移动到新灯 xAnchor，新灯开始接收 PTZ。

### 5.2 独立调试模式

`standaloneTrackDebug=true` 时，视觉节点无需灯节点 HTTP 请求即可运行追踪：每 `trackDebugInterval`（默认 200ms）读一次 HUSKYLENS，按人位置在已注册灯节点/后端目标中就近选灯推送 PTZ（无候选灯时回退到当前 ROI 绑定活动灯）。此模式用于开发调试和无灯节点场景。

---

## 6. 坐标映射数学（height 自适应算法）

### 6.1 三个坐标系

| 坐标系 | 原点 | X 轴 | Y 轴 | 单位 |
|--------|------|------|------|------|
| **图像系** | 画面左上角 (0,0) | 右 0→320 | 下 0→240 | px |
| **物理系** | 前滑轨(视觉滑轨)中点地面投影 | 右 | 远离展示墙（深度） | mm |
| **云台系** | 云台旋转中心 | Pan: 右+ 左- | Tilt: 上+ 下- | ° |

### 6.2 已知常量（`app_config.h`）

| 符号 | 值 | 含义 |
|------|-----|------|
| HUSKY_FRAME_W | 320 px | HUSKYLENS 画面宽度 |
| HUSKY_FRAME_H | 240 px | HUSKYLENS 画面高度 |
| HUSKY_FRAME_CX | 160 px | 画面水平中心 |
| HUSKY_FRAME_CY | 120 px | 画面垂直中心 |
| D_MIN_MM | 800 mm | 测距下限 |
| D_MAX_MM | 6000 mm | 测距上限 |
| H_PX_MIN | 20 px | height 像素下限 |
| H_PX_MAX | 240 px | height 像素上限 |
| H_OUTLIER_RATIO | 0.35 | height 帧间突变阈值(35%) |

### 6.3 可配置参数（`DeviceConfig`）

| 参数 | 默认值 | 含义 | 范围 |
|------|--------|------|------|
| trackAlgorithm | 0 | HUSKYLENS 算法模式: 0=人脸识别(自动触发), 1=物体追踪(按键学习) | 0/1 |
| yPerson | 2000 mm | 客户站立深度（**不固定**，仅作 height 无效时的降级备用深度） | 1000~5000 |
| dRails | 200 mm | 前后滑轨间距 | 200~1500 |
| railHeightDiff | 200 mm | 后滑轨(灯)相对前滑轨(摄像头)高度差，正值=灯更高（实际安装前滑轨低 20cm） | -500~500 |
| camHeightMm | 1600 mm | 前滑轨(摄像头)离地高度（2026-08-18 实测, 2026-08-19 复核修正）；灯离地 = camHeightMm + railHeightDiff = 1800mm | 800~3000 |
| eyeHeightRatio | 0.93 | 人脸框中心(眼位)高度 ≈ 0.93 × 身高（人体测量学比例） | 0.85~0.98 |
| chestHeightRatio | 0.74 | 胸口照射点高度 ≈ 0.74 × 身高（人体测量学比例） | 0.60~0.85 |
| tiltOffsetDeg | 30.0° | 灯云台仰角偏移，正值=抬高照射方向；最终 tilt_A = 几何计算值 + 偏移（2026-08-19 新增，实测云台仰角偏低、光斑照偏下的机械零位补偿） | -45~45 |
| deadZone | 3% | 死区百分比（deprecated，无云台B 不再使用，保留兼容配置 API） | 1~20 |
| focalPx | 280.0 | 有效焦距(像素)，一步标定得出 | 100~600 |
| personHeight | 1700 mm | 假定人真实身高（**仅物体追踪模式全身框测距用**，人脸模式身高在线反推） | 1000~2200 |
| faceHeight | 240 mm | 人脸框真实高度（人脸识别模式测距用） | 100~500 |
| hSmoothAlpha | 0.3 | height EMA 平滑系数(越小越平滑) | 0.05~1.0 |
| standaloneTrackDebug | true | 独立调试模式 | — |
| trackDebugInterval | 200 ms | 独立模式输出间隔 | 50~5000 |

> `trackAlgorithm` 变更后固件立即调用 `applyHuskyAlgorithm()` 经 I2C 切换二哈算法；非法值回退为 0（人脸识别）。

### 6.4 height 自适应测距

核心改进：不再假设固定距离 `yPerson`，而是通过 HUSKYLENS 检测框的 `height`（人像像素高度）反推实际距离。

```
输入: HUSKYLENS {xCenter, yCenter, width, height}
输出: 云台B {pan_B, tilt_B} + 灯云台A {pan_A, tilt_A} + 距离 {distCam, distLamp}

Step 1: height 平滑（突变剔除 + EMA）
  if height < H_PX_MIN(20) 或 > H_PX_MAX(240):  降级到 yPerson + dRails
  if |height - hPxLast| / hPxLast > 0.35:        丢弃本帧（噪声剔除）
  hSmooth = hSmoothAlpha × height + (1 - hSmoothAlpha) × hPxSmoothed

Step 2: height 像素 → 摄像头距离（小孔成像逆运算）
  boxRealHeight = trackAlgorithm==1 ? personHeight : faceHeight
  D_cam = boxRealHeight × focalPx / hSmooth
  D_cam = constrain(D_cam, D_MIN_MM, D_MAX_MM)

Step 3: 精确角度（atan2，非近似）
  θ_x = atan2(xCenter - 160, focalPx)
  θ_y = atan2(yCenter - 120, focalPx)
  pctX = (xCenter - 160) / 160
  pctY = (yCenter - 120) / 120

Step 4: 物理水平偏移（精确小孔）
  ΔX = (xCenter - 160) × D_cam / focalPx

Step 5: 灯深度（灯靠客户侧，比摄像头近 dRails）
  D_lamp = D_cam - dRails
  D_lamp = max(D_lamp, D_MIN_LAMP_MM=300)

Step 6: 绝对高度瞄准模型（2.5.0 射灯照胸口；当前无云台B，pan_B/tilt_B 用于状态上报与 tilt_A 派生）
  pan_B  = θ_x × 180/π                              (约束 ±90°)
  // 检测框中心绝对高度: 摄像头离地 camHeightMm, θ_y>0 = 目标在摄像头水平面下方
  boxCenterAbs = camHeightMm − D_cam × tan(θ_y)
  // 人高/站位不固定 → 按算法模式由 boxCenterAbs 在线反推胸口绝对高度:
  //   人脸识别:   框中心=眼位 ≈ eyeHeightRatio×身高 → 反推身高再取胸口比例
  //               chestAbs = boxCenterAbs × chestHeightRatio / eyeHeightRatio
  //   物体追踪:   框=全身, 框中心≈0.5×身高
  //               chestAbs = boxCenterAbs + (chestHeightRatio − 0.5) × boxRealHeight
  chestAbs = constrain(chestAbs, 300, 2000)
  // tilt_B = 摄像头指向胸口点的俯仰角 (负=下俯), 不再使用固定角偏移
  tilt_B = −atan2(camHeightMm − chestAbs, D_cam) × 180/π   (约束 ±45°)

Step 7: 灯云台A 角度（视差修正 + 自适应距离 + 高度差修正）
  Δx_lamp = ΔX + (rearPosition - xAnchor)
  pan_A   = atan2(Δx_lamp, D_lamp) × 180/π   (约束 ±90°)
  // 高度差修正 (2.4.0): 前滑轨比后滑轨低 railHeightDiff (灯更高)
  chestRelCam  = D_cam × tan(tilt_B)            // 胸口相对摄像头的高度 (= chestAbs − camHeightMm)
  chestRelLamp = chestRelCam − railHeightDiff   // 换算到灯基准 (灯高 → 相对更低)
  tilt_A  = atan2(chestRelLamp, D_lamp) × 180/π  (约束 ±45°)
```

> **绝对高度模型数值示例**（身高 1700mm，眼位 0.93×1700=1581mm，胸口 0.74×1700=1258mm，camHeightMm=1600，railHeightDiff=200，灯离地 1800mm）：
>
> | D_cam | chestAbs | tilt_B | tilt_A | 光斑落点高度 |
> |-------|----------|--------|--------|--------------|
> | 2200mm | 1581×0.74/0.93 ≈ 1258mm | −atan2(342, 2200) ≈ −8.8° | atan2(−542, 2000) ≈ −15.2° | 1800 − 2000×tan(15.2°) ≈ **1258mm（胸口）** |
> | 4000mm | 1258mm（身高不变则不变） | −atan2(342, 4000) ≈ −4.9° | atan2(−542, 3800) ≈ −8.1° | 1800 − 3800×tan(8.1°) ≈ **1258mm（胸口）** |
>
> 对比旧固定角偏移模型（tilt 恒 −20°）：D_cam=4000mm 时落点 = 1800 − 3800×tan(20°) ≈ **417mm（小腿）**，偏差超过 800mm。新模型落点只取决于 chestAbs，与距离无关。

> `yPerson` 保留作降级备用：当 height 像素超出有效范围 [20, 240] 时，`D_cam` 回退到 `yPerson + dRails`。
>
> 人脸识别模式下未学习人脸以 ID=0 输出，固件同样接受（识别到人即自动触发）；物体追踪模式仅接受已学习目标（ID≥1）。

### 6.5 一步标定（`/track/calibrate`）

让人站在已知距离 `calibDistMm` 处，固件用当前检测框 `height` 反推有效焦距：

```
boxRealHeight = trackAlgorithm==1 ? personHeight : faceHeight
focalPx = calibDistMm × hPx_raw / boxRealHeight
```

标定结果持久化到 LittleFS，后续追踪使用新 `focalPx`。

### 6.6 就近选灯（2.3.0 新增）

追踪中人的世界坐标 X（沿滑轨方向）由相机位置与水平偏移合成：

```
personX = rearPosition + ΔX          // rearPosition=前滑轨位置, ΔX=人相对相机的物理偏移
```

视觉节点每帧在候选灯中选 xAnchor 离 personX 最近的一盏：

| 候选来源 | xAnchor 来源 |
|---------|-------------|
| 已注册灯节点 `lampNodes[]` | 灯节点 POST `/track/start` 时携带（心跳保活 15s） |
| 后端 `/lamp-ip` 目标 `g_lampTargets[]` | JSON 可选字段 `xAnchor`（按 IP 与注册灯去重） |

- **滞回**：新灯距离需比当前灯近 `NEAREST_LAMP_HYSTERESIS_MM=150` 以上才切换，防止人在两区交界处来回跳灯。
- **切换动作**：旧灯收到 `tracking:false`（灯端回中/恢复默认）→ 前滑轨移动到新灯 `xAnchor`（保持人在画面中央）→ 按新 `xAnchor` 重算 `pan_A`（视差修正 `Δx_lamp = ΔX + (rearPosition − xAnchor)`）→ 推送新灯。
- **回退**：无候选灯时退回旧行为——推送当前 ROI 绑定的活动灯 `g_activeLampIp`。
- 追踪停止/回中/新 `/track/start` 时调用 `resetNearestLamp()` 清空选择状态。

---

## 7. 固件项目结构

### 7.1 固件项目清单

```
8266-master/                        # 视觉节点 ESP8266 #1
├── platformio.ini                  #   ESP-12E + HUSKYLENS + WebSocket
├── src/main.cpp                    #   主循环 + 追踪状态机 + UART 拍照触发
├── src/device/
│   ├── arm_controller.cpp          #   Nano 云台/滑轨串口控制 + 摇杆运动
│   ├── person_tracker.cpp          #   height 自适应坐标转换算法
│   ├── light_control.cpp           #   双色温 PWM + 呼吸灯 + 灯效
│   ├── sensor_manager.cpp          #   ToF + HUSKYLENS 初始化
│   └── ota_manager.cpp             #   OTA 固件升级
├── src/network/
│   ├── track_server.cpp            #   HTTP API (/track/*, /ping, /track/calibrate)
│   ├── lamp_notify.cpp             #   HTTP POST /lamp/control (带变化检测节流)
│   ├── lamp_manager.cpp            #   灯节点注册 + 心跳管理 (最多3个, 15s超时)
│   ├── ws_client.cpp               #   WebSocket 双向通信 (状态/特效/云台/OTA)
│   ├── http_reporter.cpp           #   HTTP 状态上报
│   ├── wifi_manager.cpp            #   WiFi 连接 + SmartConfig 配网
│   ├── mdns_manager.cpp            #   mDNS (<deviceId>.local)
│   └── udp_discovery.cpp           #   UDP 广播 :4210
├── src/server/
│   └── local_server.cpp            #   本地 HTTP API (/setLight, /lamp-ip, /resetWifi...)
├── src/config/
│   └── config_manager.cpp          #   LittleFS 配置持久化
└── include/app_config.h            #   引脚定义 + 常量 + DeviceConfig 结构体

lamp-node/                          # 灯节点 ESP8266 #2/#3/#4
├── platformio.ini                  #   ESP-12E (精简依赖, 无 HUSKYLENS/WS/OTA)
└── src/main.cpp                    #   单文件固件 (ToF 状态机 + HTTP + mDNS)

esp32-cam-firmware/                 # ESP32-S3-CAM 拍照上传
├── platformio.ini                  #   ESP32-S3 + OV3660 + esp32-camera
├── src/main.cpp                    #   UART 命令解析 + 拍照 + HTTP multipart 上传
└── HARDWARE_NOTES.md               #   OV3660 硬件接线说明 + I2C Slave 规划
```

### 7.2 视觉节点 (#1) — 功能模块

```
ESP8266 #1（视觉节点）
├── 📷 HUSKYLENS (I2C 0x32, D5/D6, 400kHz)
├── 📏 VL53L0X ToF (I2C 0x29, 共享总线)
├── 📸 ESP32-S3-CAM (UART Serial1 GPIO2 TX → ESP32-S3 GPIO7 RX, 115200)
├── 前滑轨电机 (Nano 串口 57600)
├── 🔦 双色温 LED (D2/D1) + 散光 BLUR (D7) [历史保留]
├── 📡 WiFi HTTP Server (:80)
├── 📡 WebSocket Client (→后端 /ws/device)
├── 📡 mDNS (<deviceId>.local)
└── 职责:
    • HTTP API: /track/start, /track/stop, /ping, /track/status,
                 /track/config, /track/calibrate, /setLight, /lamp-ip, ...
    • UDP 广播 :4210 (设备发现)
    • 上电切换 HUSKYLENS 为人脸识别模式 (自动检测, 无需按键; 可配置切回物体追踪)
    • 读取 HUSKYLENS 坐标 → height 自适应测距 + atan2 坐标转换 (测距常数随算法模式选择)
    • 驱动前滑轨(视觉滑轨, 0~1600mm) 到位 (无云台B, HUSKYLENS 固定正对人)
    • 检测到人即就近选灯 (150mm 滞回) 并 HTTP POST /lamp/control 下发灯云台A PTZ (灯端变化检测节流)
    • 管理灯节点注册/心跳 (最多3个, 15s超时)
    • 支持服务器下发的 lampIps[] 列表 (向所有灯广播 PTZ)
    • 追踪生命周期中经 UART 触发 ESP32-S3 拍照
```

### 7.3 灯节点 (#2/#3/#4) — 功能模块

```
ESP8266 #2/#3/#4（灯节点）
├── 🔦 双色温 LED (冷白 D2/暖白 D1 PWM)
├── 🎛 灯云台A (Pan ±90°, Tilt ±45°, Nano 串口 57600)
├── 📏 VL53L0X ToF (I2C 0x29) — 2m 触发, 1s 进入防抖, 0.8s 离开防抖
├── 💡 BH1750 光照 (I2C 0x23) — 环境光自适应 (仅 IDLE 状态)
├── 📡 WiFi HTTP Server (:80)
│   ├── POST /lamp/control  — 接收视觉节点 PTZ
│   ├── GET  /status        — 状态查询
│   ├── POST /setLight      — 手动灯光控制
│   └── POST /resetWifi     — 清除配置
├── 📡 WiFi HTTP Client
│   ├── POST /track/start   — 触发追踪
│   ├── POST /track/stop    — 停止追踪
│   └── POST /ping          — 心跳 (每5s)
├── 📡 mDNS (<lampId>.local)
├── 📡 UDP :4210 监听 (发现视觉节点)
└── 状态机: IDLE → TRACKING_START → TRACKING → IDLE
```

### 7.4 ESP32-S3-CAM — 功能模块

```
ESP32-S3-WROOM-1U + OV3660
├── 📸 OV3660 摄像头 (XGA 1024x768, JPEG quality 12, PSRAM 必需)
├── 💡 状态 LED (GPIO1, 慢闪=待机, 快闪=忙碌)
├── 📡 UART2 (RX=GPIO7, TX=GPIO8, 115200) — 接收视觉节点文本命令
├── 📡 SCCB (GPIO4=SDA, GPIO5=SCL) — OV2640 配置 (esp_camera 库管理)
├── 📡 WiFi HTTP Client
│   └── POST /api/camera/upload (multipart JPEG + 元数据)
└── 状态机: IDLE → CAPTURING → UPLOADING → IDLE
```

---

## 8. ESP32-S3-CAM UART 命令协议

### 8.1 物理连接

```
ESP8266 #1 Serial1 (GPIO2, 仅 TX, 115200 baud)
  ──────→ ESP32-S3 UART2 RX (GPIO7)

命令格式: 文本行，首字符为命令码，后接参数，以 '\n' 结尾
```

### 8.2 命令表

| 命令 | 含义 | 触发时机 |
|------|------|---------|
| `C` | 立即拍照 + HTTP multipart 上传 | 追踪开始/中/结束 |
| `Z{zone}` | 设置区域元数据 | `Zleft` / `Zcenter` / `Zright` |
| `Tstart` | 设置触发类型 = track_start | 追踪开始 |
| `Ttrack` | 设置触发类型 = tracking | 追踪中（每 3s） |
| `Tstop` | 设置触发类型 = track_stop | 追踪停止 |
| `Tmanual` | 设置触发类型 = manual | 手动 |
| `S` | 重置触发类型为 manual | 追踪停止后 |
| `R` | 状态查询（回复 `OK wifi=x zone=y`） | 调试 |

### 8.3 视觉节点中的触发点

| 触发点 | 位置 | 发送命令 |
|--------|------|---------|
| 追踪开始 | `track_server.cpp` `handleTrackStart()` | `Z{zone}` → `Tstart` → `C` |
| 灯主动停止 | `track_server.cpp` `handleTrackStop()` | `Tstop` → `C` → `S` |
| 追踪中定时 | `main.cpp` 追踪循环（每 3s） | `Ttrack` → `C` |
| 人丢失停止 | `main.cpp` 丢失回中 | `Tstop` → `C` → `S` |

### 8.4 上传元数据

ESP32-S3-CAM 收到 `C` 命令后，使用当前 `g_zone` / `g_trigger` 作为上传元数据：

```
POST /api/camera/upload  (multipart/form-data)
  image:      <JPEG binary, XGA 1024x768>
  deviceId:   "cam-XXXXXXXX"          (ESP32-S3 EFUSE MAC)
  zone:       "left" / "center" / "right"
  trigger:    "track_start" / "tracking" / "track_stop" / "manual"
  timestamp:  <millis()>
```

---

## 9. HTTP API 参考

### 9.1 视觉节点 HTTP API (ESP8266 #1)

| 端点 | 方法 | 请求方 | 说明 |
|------|------|--------|------|
| `/track/start` | POST | 灯节点 | `{zone, lampId, xAnchor}` → 开始追踪 |
| `/track/stop` | POST | 灯节点 | `{zone, lampId}` → 停止追踪 |
| `/ping` | POST | 灯节点 | `{lampId, zone}` → 心跳 (15s超时) |
| `/track/status` | GET | 调试 | 追踪状态 JSON (含 distCam/distLamp/hPx/faceHeightAbs/chestHeightAbs 等) |
| `/track/config` | POST | 调试 | 更新 trackAlgorithm/faceHeight/yPerson/dRails/railHeightDiff/camHeightMm/eyeHeightRatio/chestHeightRatio/tiltOffsetDeg/deadZone/focalPx/personHeight/hSmoothAlpha |
| `/track/calibrate` | POST | 调试 | 一步标定 focalPx `{calibDistMm, faceHeight?/personHeight?}`（按算法模式选择框高度） |
| `/lamp-ip` | POST | 后端 | 服务器下发射灯映射 `{targets:[{targetIndex,targetChipId,lampIp,xAnchor?}], lampIps:[...]}` |
| `/status` | GET | 调试 | 视觉节点状态 |
| `/setLight` | POST | 手动控制 | 灯光参数 |
| `/stopBroadcast` | POST | 调试 | 停止 UDP 广播 |
| `/resumeBroadcast` | POST | 调试 | 恢复 UDP 广播 + 上报 |
| `/stopAnnounce` | POST | 调试 | 停止状态上报 |
| `/resetWifi` | POST | 配网 | 清除配置并重启 |

### 9.2 灯节点 HTTP API (ESP8266 #2/#3/#4)

| 端点 | 方法 | 请求方 | 说明 |
|------|------|--------|------|
| `/lamp/control` | POST | 视觉节点 | `{tracking, pan, tilt, brightness, temp, personData}` |
| `/status` | GET | 调试 | 灯节点状态 JSON |
| `/setLight` | POST | 手动控制 | `{brightness, colorTemp, autoMode, ...}` |
| `/resetWifi` | POST | 配网 | 清除配置并重启 |

### 9.3 WebSocket 消息（视觉节点 ↔ 后端）

视觉节点连接 `ws://{serverHost}:{wsPort}/ws/device`，双向通信：

**视觉节点 → 后端**

| type | 触发时机 | 内容 |
|------|---------|------|
| `register` | 连接建立 | `{id, chipId, deviceType, fwVersion, ip, mac, ...}` |
| `ping` | 每 5s | `{id, chipId}` |
| `personDetection` | 检测到人 | `{xCenter, yCenter, width, height, pctX, pctY, deltaX, panB, tiltB}` |
| 状态上报 | 定时/事件 | 设备状态 JSON |

**后端 → 视觉节点**

| type | 含义 | 关键字段 |
|------|------|---------|
| `state` / `control` | 灯光参数控制 | `{brightness, temp, autoMode, recommendedBrightness, fabric, ...}` |
| `effect` | 灯效控制 | `{effect:"wave", enabled, baseTemp, range, speed, brightness, phaseOffset}` |
| `locate` | 呼吸定位 | `{times, duration}` |
| `arm_joystick` | 摇杆运动 | `{x, y, durationMs}` |
| `arm_stop` | 停止摇杆 | — |
| `arm_position` | 绝对位置 | `{pan, tilt, slider}` |
| `arm_speed` | 速度档位 | `{speed:"slow"/"normal"/"fast"}` |
| `arm` | 动作命令 | `{action:"up"/"down"/"left"/"right"/"center"/"home"/...}` |
| `person_track` | 开关追踪 | `{enabled}` |
| `person_track_config` | 追踪参数 | `{trackAlgorithm, faceHeight, yPerson, dRails, railHeightDiff, camHeightMm, eyeHeightRatio, chestHeightRatio, tiltOffsetDeg, deadZone, focalPx, personHeight, hSmoothAlpha}` |
| `ota_update` | OTA 升级 | `{version, url, versionCode, channel, md5}` |
| `command` | 通用命令 | `{cmd:"resume_broadcast"}` |

### 9.4 `/lamp/control` 请求体

```json
{
  "tracking": true,
  "pan": -11.1,
  "tilt": -18.1,
  "brightness": 80,
  "temp": 4000,
  "personData": {
    "xCenter": 100,
    "yCenter": 130,
    "width": 133,
    "height": 135,
    "pctX": -0.375,
    "deltaX": -433,
    "rearPosition": 190,
    "distCam": 2200,
    "distLamp": 2000
  }
}
```

> `pan`/`tilt` 为灯云台A 角度（`pan_A`/`tilt_A`），非云台B 角度。`tracking:false` 时仅发 `{"tracking":false}` 通知停止。视觉节点带变化检测节流：pan/tilt 变化 ≤0.3° 或亮度/色温不变时跳过推送。

### 9.5 灯 IP 分发模式

视觉节点支持多种灯 IP 来源，2.3.0 起**就近选灯**为默认分发策略：

1. **就近选灯（默认）**：每帧按 `personX = rearPosition + ΔX` 在候选灯中选最近一盏（候选 = 已注册灯节点 `lampNodes[]` + 后端 `/lamp-ip` 下发且带 `xAnchor` 的目标灯），只向选中灯推送 PTZ；150mm 滞回防抖，切换时旧灯收到 `tracking:false`。见 §6.6。
2. **活动灯回退**：无候选灯（无注册灯、后端目标未带 xAnchor）时，推送当前 ROI 绑定的活动灯 `g_activeLampIp`。
3. **灯节点注册（遗留）**：灯节点 POST `/track/start` 时自动注册 IP 与 xAnchor，成为就近选灯的候选来源。
4. **全灯广播（遗留）**：后端 `/lamp-ip` 传入 `lampIps[]` 数组时，`notifyAllLamps` 向所有 IP 广播 PTZ（当前代码路径已不再调用）。

---

## 10. 防抖策略

| 机制 | 参数 | 说明 |
|------|------|------|
| 丢失回中 | 3 秒无人 | 前滑轨回 800mm，HTTP 通知灯停止（`TRACK_LOST_TIMEOUT_MS`） |
| 输出节流 | 100ms | HTTP 推送 PTZ 数据间隔（`TRACK_PUSH_INTERVAL_MS`） |
| 变化检测节流 | pan/tilt ≤0.3° 不推送 | 减少约 80% 流量（`lamp_notify.cpp`） |
| 就近选灯滞回 | 150mm | 新灯需比当前灯近 150mm 以上才切换（`NEAREST_LAMP_HYSTERESIS_MM`） |
| ToF 触发防抖 | 1s | 过滤路过/短暂停留（`TOF_DEBOUNCE_MS`） |
| ToF 离开防抖 | 0.8s | 避免频繁开关追踪（`TOF_LOST_DEBOUNCE_MS`，灯节点） |
| height 突变剔除 | 35% | 帧间 height 变化 >35% 丢弃（`H_OUTLIER_RATIO`） |
| height EMA 平滑 | hSmoothAlpha 0.3 | 指数移动平均，平滑测距波动 |

---

## 11. 色温映射

```
色温范围: 2700K (全暖白) ~ 6500K (全冷白)

tempVal = map(temp, 2700, 6500, 0, 1024)      // 色温 → 0~1024
briVal  = map(brightness, 0, 100, 0, 1024)    // 亮度 → 0~1024

pwmCold = tempVal × briVal / 1024             // 冷白占空
pwmWarm = (1024 - tempVal) × briVal / 1024    // 暖白占空

analogWrite(LED_COLD_PIN, 1024 - pwmCold)     // LED 低电平驱动，取反
analogWrite(LED_WARM_PIN, 1024 - pwmWarm)
```

灯节点支持 2s 平滑过渡（`LIGHT_SMOOTH_MS`），收到新亮度/色温后在 `loop()` 中渐变到位。

---

## 12. 角度范围

| 轴 | 最小值 | 最大值 | 说明 |
|----|--------|--------|------|
| 云台B Pan | -90° | +90° | 算法仍计算（状态上报），当前部署未安装云台B |
| 云台B Tilt | -45° | +45° | 算法仍计算（派生 tilt_A），当前部署未安装云台B |
| 灯云台A Pan | -90° | +90° | 灯光水平照射 |
| 灯云台A Tilt | -45° | +45° | 灯光垂直角度 |
| 前滑轨 (视觉滑轨) | 0mm | 1600mm | 三区移动范围（回中位置 800mm） |

---

## 13. 配置参数

### 13.1 视觉节点 config.json

```json
{
  "ssid": "NaHS",
  "password": "123456789",
  "serverHost": "device.genius.show",
  "httpPort": 80,
  "wsPort": 80,
  "staticIP": "",
  "gateway": "",
  "subnetMask": "",
  "personTrackEnabled": false,
  "yPerson": 2000,
  "dRails": 200,
  "railHeightDiff": 200,
  "deadZone": 3,
  "standaloneTrackDebug": true,
  "trackDebugInterval": 200,
  "trackAlgorithm": 0,
  "focalPx": 280.0,
  "personHeight": 1700,
  "faceHeight": 240,
  "hSmoothAlpha": 0.3,
  "camHeightMm": 1600,
  "eyeHeightRatio": 0.93,
  "chestHeightRatio": 0.74,
  "tiltOffsetDeg": 30.0
}
```

> `deviceId` 由 `ESP.getChipId()` 自动生成，格式 `LAMP-XXXXXX`（历史命名，视觉节点复用此格式）。`FW_DEVICE_TYPE` = `"cam"`。

### 13.2 灯节点 config.json (区分配置)

```json
{
  "ssid": "NaHS",
  "password": "123456789",
  "staticIP": "",
  "gateway": "",
  "subnetMask": "",
  "lampId": "lamp-left",
  "zone": "left",
  "xAnchor": 400,
  "visionIP": ""
}
```

> `visionIP` 为手动指定视觉节点 IP（UDP 发现的备用方案）。`lampId` 默认 `lamp-{chipId hex}`。

**三区对照**：

| 参数 | 左区 #2 | 中区 #3 | 右区 #4 |
|------|---------|---------|---------|
| `lampId` | `lamp-left` | `lamp-center` | `lamp-right` |
| `zone` | `left` | `center` | `right` |
| `xAnchor` | 400 | 800 | 1200 |

### 13.3 ESP32-S3-CAM config.json

```json
{
  "ssid": "NaHS",
  "password": "123456789",
  "serverHost": "device.genius.show",
  "serverPort": 80,
  "deviceId": "cam-AB12CD34"
}
```

> `deviceId` 默认 `cam-{EFUSE MAC hex}`，存储在 SPIFFS。

---

## 14. 编译与烧录

### 14.1 前提条件

- PlatformIO CLI 或 VS Code PlatformIO 扩展
- ESP8266 驱动 (CH340/CP2102)
- ESP32-S3 驱动 (USB-JTAG GPIO19/20 或外接 USB-TTL)

### 14.2 编译命令

```bash
# 视觉节点 (ESP8266 #1)
cd 8266-master
pio run                  # 编译
pio run -t upload        # 烧录
pio device monitor       # 串口监视 (57600)

# 灯节点 (ESP8266 #2/#3/#4)
# 烧录前修改 config.json 中的 lampId/zone/xAnchor
cd lamp-node
pio run -t upload

# ESP32-S3-CAM
cd esp32-cam-firmware
pio run -t upload        # 烧录 (需按住 IO0 进入下载模式)
```

### 14.3 ESP32-S3-CAM 烧录注意事项

1. 首次烧录用 USB 接 GPIO19(TX)/GPIO20(RX)，或模组自带 USB 口
2. 烧录时按住 IO0 按钮 → 上电 → 松开 IO0
3. 烧录完成后按 RST 重启
4. 首次运行自动创建 SPIFFS 分区
5. PSRAM 必需：无 PSRAM 时 OV3660 在 XGA 下无法运行，固件会 3 秒后重启

### 14.4 部署顺序

1. 烧录 ESP8266 #1 (视觉节点) → 验证 HUSKYLENS + 云台B + 滑轨
2. 烧录 ESP32-S3-CAM → 验证拍照 + 上传
3. 烧录 ESP8266 #2 (左区灯) → 验证 ToF + 灯 + 云台 + /track/start
4. 烧录 ESP8266 #3 (中区灯) → 同上
5. 烧录 ESP8266 #4 (右区灯) → 同上
6. 部署 camera-server 后端 → 验证 YOLO 检测 + WebSocket 推送
7. 全系统联调 → 完整追踪流程测试

---

## 15. 已知限制与待实施项

| 项目 | 现状 | 影响/改进方向 |
|------|------|--------------|
| 云台B 未安装 | 当前部署无云台B，HUSKYLENS 固定正对人；`pan_B`/`tilt_B` 仍计算用于状态上报与 `tilt_A` 派生 | 如需摄像头主动追踪可后续加装；追踪防抖由灯端 `notifyLamp` 变化检测承担 |
| ESP32-S3-CAM 走 UART 而非 I2C | 当前实现 UART 单向（视觉节点只发不收） | `HARDWARE_NOTES.md` 规划了 I2C Slave @0x08 方案（GPIO6/7），尚未实施 |
| 源码注释标注 OV2640 | 实际硬件为 OV3660，引脚配置按 OV3660 接线，`esp_camera` 库自动适配 | `main.cpp` / `platformio.ini` 头部注释滞后未更新，不影响运行 |
| UDP 发现字段不匹配 | 视觉节点广播 `{"device":"cam"}`，灯节点过滤 `{"deviceType":"vision"}` | 灯节点无法经 UDP 自动发现视觉节点，需手动配置 `visionIP` |
| HUSKYLENS 单目标追踪 | 多人场景只取 height 最大的目标 | 升级多目标追踪算法 |
| 人脸识别仅检测正脸 | 2.3.0 自动触发依赖人脸识别模式，顾客转身/侧脸时检测短暂中断 | 全身背影跟踪需 HUSKYLENS 2 (SEN0638) 物体分类；或切回物体追踪模式 (`trackAlgorithm=1`) 手动学习 |
| 就近选灯依赖灯坐标 | 候选灯需注册 (`/track/start` 带 xAnchor) 或后端 `/lamp-ip` 提供 xAnchor | 未提供 xAnchor 的目标灯不参与就近选择，回退单活动灯 |
| HTTP 轮询延迟 ~50ms | 灯光跟随有微小滞后 | 改用 UDP 广播可降到 ~5ms |
| ESP32-S3-CAM 非实时视频流 | 每张照片 ~0.5-1s 延迟 | 后期可改为 MJPEG 视频流 |
| D6/GPIO12 boot-strapping 风险 | I2C SCL 接 MTDI 引脚 | 如遇启动失败，改用 D1/D2 |
| 灯节点离开防抖 0.8s | 与视觉节点 3s 丢失超时不同步 | 灯节点可能先于视觉节点停止追踪 |

---

## 16. 项目文件索引

```
8266-master/
├── README.md                                        # 项目说明
├── CLAUDE.md                                        # AI 助手上下文
├── platformio.ini                                   # 视觉节点构建配置
├── src/                                             # 视觉节点源码
├── include/                                         # 视觉节点头文件
│   └── app_config.h                                 # 引脚 + 常量 + DeviceConfig 结构体
├── docs/
│   ├── SYSTEM_DESIGN.md                             # 本文档
│   ├── firmware_modules.md                          # 固件模块说明
│   └── superpowers/
│       ├── plans/
│       │   ├── 2026-05-30-esp32-cam-s3-server-plan.md
│       │   └── 2026-05-31-person-tracking.md
│       └── specs/
│           ├── 2026-06-04-distributed-ptz-tracking-design.md
│           └── 2026-06-04-implementation-summary.md
│
├── lamp-node/                                       # 灯节点固件
│   ├── platformio.ini
│   └── src/main.cpp
│
└── esp32-cam-firmware/                              # ESP32-S3-CAM 固件
    ├── platformio.ini
    ├── src/main.cpp
    └── HARDWARE_NOTES.md                            # OV3660 硬件接线说明 + I2C Slave 规划
```
