# ESP8266 OTA 固件模块说明

> 版本: 1.0.2 | commit: `8d119f1` | 验证日期: 2026-05-22

## 目录结构

```
E:\8266_OTA\
├── include/
│   ├── app_config.h              # 共享常量、类型、extern 声明
│   ├── config/
│   │   └── config_manager.h      # 配置读写
│   ├── network/
│   │   ├── wifi_manager.h        # WiFi 连接与配网
│   │   ├── http_reporter.h       # HTTP 上报
│   │   ├── ws_client.h           # WebSocket 客户端
│   │   └── udp_discovery.h       # UDP 广播发现
│   ├── device/
│   │   ├── light_control.h       # 灯光 PWM 控制
│   │   ├── sensor_manager.h      # ToF / BH1750 传感器
│   │   ├── ota_manager.h         # OTA 固件升级
│   │   └── arm_controller.h      # 云台/滑轨 Nano 控制
│   └── server/
│       └── local_server.h        # 本地 HTTP Server
├── src/
│   ├── main.cpp                  # 入口: setup() / loop()
│   ├── config/
│   │   └── config_manager.cpp
│   ├── network/
│   │   ├── wifi_manager.cpp
│   │   ├── http_reporter.cpp
│   │   ├── ws_client.cpp
│   │   └── udp_discovery.cpp
│   ├── device/
│   │   ├── light_control.cpp
│   │   ├── sensor_manager.cpp
│   │   ├── ota_manager.cpp
│   │   └── arm_controller.cpp
│   └── server/
│       └── local_server.cpp
├── platformio.ini
└── .gitignore
```

## 模块职责

### 1. `app_config.h` — 共享基础设施

所有模块的共同依赖，不包含业务逻辑。

- 系统头文件 (ESP8266WiFi, WebSocketsClient, ArduinoJson 等)
- 引脚定义 (`LED_COLD_PIN`, `LED_WARM_PIN`, `BLUR`, `TOF_SDA_PIN`, `TOF_SCL_PIN`)
- 固件信息 (`FW_VERSION`, `FW_VERSION_CODE`, `FW_CHANNEL`)
- 命名常量 (Wave 频率系数, ToF 过渡时长/防抖/最大距离, AP 密码, WS 路径等)
- 默认服务器配置 (host, HTTP/WS 端口)
- 定时参数 (上报间隔, 广播间隔, 心跳间隔, OTA 上报限频)
- `DeviceConfig` 结构体
- 所有全局状态变量的 `extern` 声明
- 硬件对象的 `extern` 声明 (BH1750, VL53L0X, WebSocketsClient, ESP8266WebServer, WiFiUDP)

实际定义在 `main.cpp` 中。

### 2. `config_manager` — 配置持久化

| 函数 | 说明 |
|------|------|
| `configPath()` | 返回配置文件路径 `/config.json` |
| `makeDeviceId()` | 生成设备唯一 ID `lamp-{芯片ID大写}` |
| `saveConfig()` | 保存 DeviceConfig 到 LittleFS JSON |
| `loadConfig()` | 从 LittleFS JSON 加载配置，返回是否有 WiFi 凭据 |
| `clearConfig()` | 删除 `/config.json` |
| `ensureConfigDefaults()` | 补全配置默认值 (serverHost, httpPort, wsPort) |

**依赖**: LittleFS, ArduinoJson, `cfg`/`deviceId` (extern)

### 3. `wifi_manager` — WiFi 连接与配网

| 函数 | 说明 |
|------|------|
| `connectWiFi()` | 通用 WiFi STA 连接 (ssid, password, timeout) |
| `connectSavedWiFi()` | 用已保存凭据连接 (15s 超时) |
| `smartConfigProvision()` | SmartConfig 批量配网 (60s 超时, 成功后自动保存) |
| `startConfigPortal()` | 启动 AP 热点配网 (192.168.4.1, 密码常量) |
| `getPortalHtml()` | 生成配网 HTML 页面 |
| `ensureWiFiReady()` | WiFi 保活: 已连接→直接返回, 否则重连已保存→SmartConfig→AP Portal 三级回退 |

**依赖**: `config_manager` (saveConfig, loadConfig, ensureConfigDefaults, clearConfig)

**配网策略**: 已保存 WiFi → SmartConfig → AP Portal

### 4. `http_reporter` — HTTP 上报

| 函数 | 说明 |
|------|------|
| `httpUrl()` | 拼接完整 HTTP URL |
| `postJsonToServer()` | 通用 JSON POST (WiFiClient + HTTPClient + 日志) |
| `sendDeviceStateReport()` | POST `/admin/device/state-report` 完整设备状态 |
| `sendAnnounce()` | POST `/admin/device/announce` 设备上线通告, 解析 `added:true` |
| `sendLightLevelToServer()` | POST `/admin/lux/create` 光照值上报 |
| `sendStayRecordToServer()` | POST `/admin/duration/create` 停留时长上报 |

**特殊处理**: `sendAnnounce` 需要解析响应体中的 `added` 字段，使用独立 HTTP POST 而非 `postJsonToServer`。

**依赖**: WiFi, HTTPClient, ArduinoJson, 所有灯光/状态 extern 变量

### 5. `ws_client` — WebSocket 通信

| 函数 | 说明 |
|------|------|
| `beginWebSocketClient()` | 初始化 WebSocket 连接, 设置重连间隔和心跳参数 |
| `sendWsRegister()` | 发送设备注册消息 (type: register) |
| `webSocketEvent()` | WebSocket 事件回调 (连接/断开/消息/PONG) |
| `handleWsMessage()` | **核心消息分发**: state, effect, locate, arm, command, ota_update |

**消息类型分发**:
- `state` / `control` → 更新灯光参数, 调用 `light_control` 和 `http_reporter`
- `effect` (wave) → 启动/停止色温波动效果
- `locate` → 触发呼吸定位灯
- `arm` → 云台/滑轨动作
- `command` (resume_broadcast) → 恢复广播
- `ota_update` → 触发 OTA 升级

**依赖**: `light_control`, `arm_controller`, `ota_manager`, `http_reporter`

### 6. `light_control` — 灯光 PWM 控制

| 函数 | 说明 |
|------|------|
| `applyLightSettings(br, tp)` | 冷暖双通道 PWM 输出, 色温 2700-6500K 映射 |
| `stopEffectWaveForManualControl()` | 手动控制时停止 Wave 灯效 |
| `locateBreath(times, cycleMs)` | 呼吸定位灯 (正弦波亮度, 可配置轮数和周期) |
| `updateEffectLoop()` | Wave 灯效周期更新 (正弦波色温循环) |
| `safeCopyFabric()` | 安全拷贝面料名称到固定大小缓冲区 |

**依赖**: PWM 引脚 (extern), `effectWaveEnabled`/`brightness`/`temp` 等状态变量

### 7. `sensor_manager` — 传感器管理

| 函数 | 说明 |
|------|------|
| `setupHardwareAndSensors()` | 初始化 GPIO, I2C, VL53L0X, BH1750, UDP |
| `updateLightingByToF()` | ToF 人体靠近检测 + 自动灯光过渡 |

**ToF 工作流程**:
1. 按 `TOF_READ_INTERVAL_MS` 节流读取
2. 超出 `TOF_MAX_RANGE_MM` 忽略
3. 1 秒防抖检测有人/无人状态变化
4. `TOF_TRANSITION_MS` 平滑过渡灯光到推荐值/默认值
5. 人离开时上报停留时长
6. 根据面料 (`polyester`) 控制 BLUR 光束

**降级**: ToF/BH1750 初始化失败时 `tofReady=false`/`bh1750Ready=false`，系统仍正常运行。

**依赖**: `light_control` (applyLightSettings), `http_reporter` (sendStayRecordToServer)

### 8. `ota_manager` — OTA 固件升级

| 函数 | 说明 |
|------|------|
| `compareVersion(a, b)` | 语义化版本比较 (x.y.z) |
| `doOtaUpdate(url, version, versionCode, channel, md5)` | 执行 OTA 下载和升级 |
| `doOtaUpdate(url, version)` | 简化 OTA 重载 (默认 channel/无 MD5) |

**OTA 流程**:
1. 校验 versionCode > 当前版本 (同 channel)
2. 断开 WebSocket → 下载固件 → MD5 校验
3. 进度回调: 5% 步长 / 3s 间隔上报状态
4. 成功 → 自动重启; 失败 → 重连 WebSocket

**依赖**: `http_reporter` (sendDeviceStateReport), `ws_client` (beginWebSocketClient, webSocket disconnect)

### 9. `arm_controller` — 云台/滑轨控制

| 函数 | 说明 |
|------|------|
| `sendNano(cmd, value)` | 通过 Serial1 发送单字符命令给 Arduino Nano |
| `pollNano()` | 读取 Nano 串口响应 |
| `sendPanTilt()` | 发送云台角度 (限制 ±90° pan, ±45° tilt) |
| `sendSlider()` | 发送滑轨位置 (0-1200mm) |
| `applyArmSpeed()` | 设置速度档位 (slow/normal/fast) |
| `handleArmAction()` | 方向控制: up/down/left/right/center/home/stop/aim_person/aim_cloth |

**协议**: UART 57600 baud, 单字符命令 + 数值 + `\n`

### 10. `local_server` — 本地 HTTP Server

| 函数 | 说明 |
|------|------|
| `addCorsHeaders()` | 设置 `Access-Control-Allow-Origin: *` |
| `addCorsHeadersWithMethods()` | CORS + Methods + Headers (用于 OPTIONS 预检) |
| `setupDeviceHttpServer()` | 注册所有 HTTP 路由 |
| `handleStatus()` | GET `/status` |
| `handleSetLight()` | POST `/setLight` (亮度/色温/自动模式/面料) |
| `handleResumeBroadcast()` | GET `/resumeBroadcast` |
| `handleStopBroadcast()` | GET `/stopBroadcast` |
| `handleStopAnnounce()` | GET `/stopAnnounce` |
| `handleResetWifi()` | POST `/resetWifi` |

**依赖**: `light_control` (applyLightSettings, stopEffectWaveForManualControl, safeCopyFabric), `config_manager` (clearConfig), `http_reporter` (sendDeviceStateReport)

### 11. `udp_discovery` — UDP 广播发现

| 函数 | 说明 |
|------|------|
| `calcBroadcastIP()` | 计算局域网广播地址 (IP \| ~mask) |
| `refreshBroadcastIP()` | 缓存广播地址 |
| `broadcastDevice()` | 每 5s 向 UDP 4210 端口广播设备信息 |

**优化**: 广播地址在 WiFi 重连时重新计算，正常运行时使用缓存。

## 模块依赖关系

```
app_config.h (无内部依赖)
  ├── config_manager ─→ LittleFS
  ├── wifi_manager ──→ config_manager
  ├── http_reporter ─→ WiFi, HTTPClient
  ├── arm_controller ─→ Serial1 (Nano UART)
  ├── light_control ──→ PWM 引脚
  ├── udp_discovery ──→ WiFiUDP
  ├── sensor_manager ─→ light_control, http_reporter
  ├── local_server ───→ light_control, config_manager, http_reporter
  ├── ota_manager ────→ http_reporter, ws_client (.cpp 层)
  └── ws_client ──────→ light_control, arm_controller, ota_manager, http_reporter
```

无循环头文件依赖。`ota_manager` ↔ `ws_client` 的互调仅在 `.cpp` 实现层发生。

## setup() 初始化顺序

```
1. Serial.begin(57600) / DEBUG_SERIAL.begin(115200)
2. makeDeviceId()              → 生成 LAMP-{CHIPID}
3. LittleFS.begin()            → 挂载文件系统
4. setupHardwareAndSensors()   → GPIO, I2C, VL53L0X, BH1750, UDP
5. loadConfig()                → 读取 config.json
6. connectSavedWiFi()          → 优先尝试已保存 WiFi
7. smartConfigProvision()      → 尝试 SmartConfig (60s)
8. startConfigPortal()         → 进入 AP 配网 (如前面都失败)
9. setupDeviceHttpServer()     → 注册 HTTP 路由
10. beginWebSocketClient()     → 连接 WebSocket 服务器
11. sendAnnounce()             → 设备上线通告
12. sendDeviceStateReport()    → 首次状态上报
```

## loop() 周期调度

```
1. server.handleClient()       → 处理 HTTP 请求
2. pollNano()                  → 读取 Nano 串口响应
3. portalMode? → return        → AP 配网模式阻塞
4. otaInProgress? → return     → OTA 期间阻塞
5. ensureWiFiReady()           → WiFi 保活 (三级回退)
6. webSocket.loop()            → WebSocket 事件轮询
7. broadcastDevice()           → UDP 广播 (每 5s)
8. updateEffectLoop()          → Wave 灯效
   或 updateLightingByToF()    → ToF 自动调光
9. WebSocket ping              → 应用层心跳 (每 5s)
10. sendAnnounce()             → 设备上线通告 (每 5s)
11. sendLightLevelToServer()   → 光照上报 (每 30s)
```

## 实机验证结果

| 项目 | 结果 |
|------|------|
| `platformio run` | ✅ 通过 |
| COM4 上传 | ✅ `esptool.py` 烧录成功, Hash verified |
| 设备 ID | ✅ `LAMP-37461B` |
| 固件版本 | ✅ `FW = 1.0.2` |
| config.json 读取 | ✅ `检测到本地配置` |
| WiFi 连接 | ✅ `连接成功 192.168.31.75` |
| HTTP announce | ✅ `200 added:true` |
| HTTP state-report | ✅ `200` |
| WebSocket 连接 | ✅ `已连接 device.genius.show:80/ws/device` |
| WebSocket register | ✅ `registerAck` |
| WebSocket ping/pong | ✅ 每 5s 正常 |
| VL53L0X 未连接降级 | ✅ `初始化失败` → `tofReady=false`, 系统正常运行 |
| BH1750 未连接降级 | ✅ `初始化失败` → `bh1750Ready=false`, 系统正常运行 |
| WDT reset / panic | ✅ 无 |
| heap 错误 | ✅ 无 |
| RAM 使用 | 49.8% (40784 / 81920 bytes) |
| Flash 使用 | 53.0% (553439 / 1044464 bytes) |

## HTTP / WebSocket / UDP / OTA / ToF / 灯光 关系

```
                    ┌──────────────────┐
                    │   后端服务器       │
                    │ device.genius.show │
                    └──┬───────┬───────┬┘
                       │       │       │
              HTTP POST│   WS  │  UDP  │
                       │       │       │
              ┌────────▼───┐ ┌─▼────┐ ┌▼──────┐
              │http_reporter│ │ws   │ │udp    │
              │ /announce   │ │client│ │discov.│
              │ /state-rpt  │ │      │ │       │
              │ /lux/create │ └──┬───┘ └───────┘
              │ /duration   │    │
              └─────────────┘    │ 消息分发:
                                 │ state → light_control
                                 │ effect → light_control
                                 │ locate → light_control
                                 │ arm → arm_controller
                                 │ ota_update → ota_manager
                                 │
              ┌──────────────────┐
              │  sensor_manager  │──→ light_control
              │  ToF 靠近检测     │──→ http_reporter
              │  BH1750 光照      │    (停留时长上报)
              └──────────────────┘
                        │
              ┌─────────▼────────┐
              │   light_control   │
              │   PWM 冷暖双通道   │
              │   Wave 灯效       │
              │   呼吸定位灯       │
              └──────────────────┘
```

- **HTTP** 单向上报 (状态/光照/停留时长/上线通告)，由 `http_reporter` 统一处理
- **WebSocket** 双向通信，服务器下发控制指令，`handleWsMessage` 分发到各模块
- **UDP** 局域网广播发现，设备每 5s 发送 JSON，地址缓存避免重复计算
- **OTA** 通过 WebSocket 接收升级通知 → HTTP 下载固件 → MD5 校验 → 自动重启
- **ToF** 靠近检测驱动自动灯光过渡，离开后上报停留时长
- **灯光** 冷暖 PWM 输出 + Wave 正弦波灯效 + locate 呼吸定位
