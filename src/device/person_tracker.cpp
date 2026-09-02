#include "device/person_tracker.h"
#include "device/arm_controller.h"

// ===================== 布局命名说明 (2026-07-21 对调) =====================
// 物理布局已对调：视觉单元 (HUSKYLENS + ESP32-S3-CAM + 云台B) 现位于【前滑轨】(靠墙侧, 可滑动)；
// 灯单元 (灯A + 云台A) 现位于【后滑轨】(靠客户侧, 固定)。
// 因此深度关系反转：视觉深度 = yPerson + dRails (靠墙更深)，灯深度 = yPerson (靠客户更近)。
// 代码中变量名 rearSliderMm / rearPosition / sendRearSlider / rear_* 等"rear"为历史命名保留，
// 不改为 vision* 以避免破坏 HTTP action (rear_left/rear_right/rear_center)、
// JSON 字段 (rearPosition) 及 Nano 串口命令 ('r'/'R') 协议契约。
// "rear" 在此文件中语义=“视觉单元所在滑轨”，物理上即前滑轨。
// =====================================================================

// ===================== 实例 =====================
HUSKYLENS huskyLens;
static bool huskyReady = false;

// ===================== height EMA 平滑状态 =====================
static float hPxSmoothed = 0.0f;
static float hPxLast     = 0.0f;
static bool  hPxInit     = false;

// ===================== 初始化 =====================
void initPersonTracker() {
  DEBUG_SERIAL.println(F("[TRACK] HUSKYLENS I2C init..."));
  int retries = 0;
  const int maxRetries = 50;
  while (!huskyLens.begin(Wire)) {
    if (++retries >= maxRetries) {
      DEBUG_SERIAL.println(F("[TRACK] HUSKYLENS FAILED"));
      huskyReady = false;
      return;
    }
    delay(200);
  }
  huskyReady = true;
  DEBUG_SERIAL.println(F("[TRACK] HUSKYLENS connected"));

  // I2C 请求超时基准 100ms (每帧读取用, 避免无人时阻塞主循环)
  huskyLens.setTimeOutDuration(100);

  // 应用算法模式: 默认人脸识别 → 识别到人自动触发 (无需在二哈上按键)
  applyHuskyAlgorithm();
}

// ===================== 应用算法模式 (自动检测 / 按键学习) =====================
// cfg.trackAlgorithm: 0=人脸识别(自动触发) 1=物体追踪(按键学习, wiki 7.2 旧行为)
bool applyHuskyAlgorithm() {
  if (!huskyReady) return false;

  int algo = cfg.trackAlgorithm;
  if (algo != 1) algo = 0;  // 非法值回退到人脸识别

  protocolAlgorithm target = (algo == 1) ? ALGORITHM_OBJECT_TRACKING
                                         : ALGORITHM_FACE_RECOGNITION;

  // 切换算法时二哈会短暂重启 (可达 1-3s), 临时放大 I2C 应答超时
  huskyLens.setTimeOutDuration(1000);

  bool ok = false;
  for (int i = 0; i < 3; i++) {
    if (huskyLens.writeAlgorithm(target)) { ok = true; break; }
    delay(200);
  }

  // 恢复每帧读取用的 100ms 超时, 避免阻塞主循环
  huskyLens.setTimeOutDuration(100);

  if (ok) {
    DEBUG_SERIAL.println(algo == 1
      ? F("[TRACK] HUSKYLENS 模式: 物体追踪 (需按键学习目标)")
      : F("[TRACK] HUSKYLENS 模式: 人脸识别 (识别到人自动触发)"));
  } else {
    DEBUG_SERIAL.println(F("[TRACK] HUSKYLENS writeAlgorithm 失败! 请在二哈菜单手动切换到人脸识别"));
  }
  return ok;
}

bool isHuskyLensReady() {
  return huskyReady;
}

// ===================== 读检测结果 =====================
bool readPersonFromHuskyLens(HUSKYLENSResult& out) {
  if (!huskyReady) return false;

  huskyLens.request();

  int bestH = 0;
  bool found = false;

  while (huskyLens.available()) {
    HUSKYLENSResult r = huskyLens.read();
    if (r.command != COMMAND_RETURN_BLOCK) continue;
    if (r.width < 8) continue;

    // ID 过滤 (2026-08 自动触发改造):
    // 人脸识别模式: 未学习的人脸 ID=0 也要接受 → 识别到人即自动触发;
    // 物体追踪模式: 仅接受已按键学习的目标 (ID>=1)。
    if (cfg.trackAlgorithm == 1 && r.ID == 0) continue;

    // 取 height 最大的目标（最近的人，与测距语义一致）
    if (r.height > bestH) {
      bestH = r.height;
      out = r;
      found = true;
    }
  }

  return found;
}

// ===================== height 平滑（突变剔除 + EMA） =====================
// 在 h_px 域平滑（非线性变换 D=K/h 前更稳定）
static float smoothHeight(float hRaw) {
  if (hRaw < H_PX_MIN || hRaw > H_PX_MAX) return hPxSmoothed;
  if (!hPxInit) { hPxSmoothed = hRaw; hPxLast = hRaw; hPxInit = true; return hRaw; }
  float ratio = fabs(hRaw - hPxLast) / hPxLast;
  if (ratio > H_OUTLIER_RATIO) { hPxLast = hRaw; return hPxSmoothed; }
  hPxSmoothed = cfg.hSmoothAlpha * hRaw + (1.0f - cfg.hSmoothAlpha) * hPxSmoothed;
  hPxLast = hRaw;
  return hPxSmoothed;
}

// ===================== 坐标转换（核心引擎 §4，height 自适应版） =====================
PersonTrackResult computeTracking(const HUSKYLENSResult& r, int xAnchor) {
  PersonTrackResult res;

  // 保存原始数据
  res.xCenter = r.xCenter;
  res.yCenter = r.yCenter;
  res.width   = r.width;
  res.height  = r.height;
  res.hPxRaw  = (float)r.height;

  // ---- Step 1: 像素偏移 → 精确 atan 角度 ----
  float thetaX = atan2((float)(r.xCenter - HUSKY_FRAME_CX), cfg.focalPx);
  float thetaY = atan2((float)(r.yCenter - HUSKY_FRAME_CY), cfg.focalPx);
  res.pctX = (float)(r.xCenter - HUSKY_FRAME_CX) / (float)HUSKY_FRAME_CX;
  res.pctY = (float)(r.yCenter - HUSKY_FRAME_CY) / (float)HUSKY_FRAME_CY;

  // ---- Step 2: height 像素 → 距离（小孔成像逆运算） ----
  // 人脸识别模式: 检测框=人脸 (~faceHeight mm)
  // 物体追踪模式: 检测框=全身 (~personHeight mm)
  float hSmooth = smoothHeight((float)r.height);
  res.hPxSmooth = hSmooth;
  float boxRealHeight = (cfg.trackAlgorithm == 1) ? cfg.personHeight : cfg.faceHeight;
  float D_cam = boxRealHeight * cfg.focalPx / hSmooth;
  D_cam = constrain(D_cam, D_MIN_MM, D_MAX_MM);
  // height 无效时降级到固定深度
  if (r.height < H_PX_MIN || r.height > H_PX_MAX) {
    D_cam = cfg.yPerson + cfg.dRails;
  }
  res.distCam = D_cam;

  // ---- Step 3: 距离 → 物理水平偏移（精确小孔） ----
  res.deltaX = (float)(r.xCenter - HUSKY_FRAME_CX) * D_cam / cfg.focalPx;

  // ---- Step 4: 视觉滑轨位置 + 灯深度 ----
  // 视觉滑轨（物理上前滑轨，变量名 rear* 为历史命名）保持 xAnchor 位置不动
  res.rearPosition = rearSliderMm;
  // 灯靠客户侧，比摄像头近 dRails
  float D_lamp = D_cam - cfg.dRails;
  D_lamp = max(D_lamp, D_MIN_LAMP_MM);
  res.distLamp = D_lamp;

  // ---- Step 5: 绝对高度瞄准模型 (2.5.0: 射灯照胸口) ----
  // pan_B 与距离 D 无关（D 被约掉），保持正确
  res.pan_B = thetaX * RAD2DEG;
  res.pan_B = constrain(res.pan_B, -90.0f, 90.0f);
  // 检测框中心绝对高度: 摄像头离地 camHeightMm, θ_y>0 = 目标在摄像头水平面下方
  // (正装假设: 画面下半部 = 光轴下方)
  float boxCenterAbs = cfg.camHeightMm - D_cam * tan(thetaY);
  float chestHeightAbs;
  if (cfg.trackAlgorithm == 1) {
    // 物体追踪模式: 检测框=全身, 框中心≈0.5×身高; 胸口点≈chestHeightRatio×身高
    chestHeightAbs = boxCenterAbs + (cfg.chestHeightRatio - 0.5f) * boxRealHeight;
  } else {
    // 人脸识别模式: 框中心=人脸(眼位≈eyeHeightRatio×身高), 反推身高再取胸口比例
    chestHeightAbs = boxCenterAbs * cfg.chestHeightRatio / cfg.eyeHeightRatio;
  }
  chestHeightAbs = constrain(chestHeightAbs, 300.0f, 2000.0f);
  res.faceHeightAbs  = boxCenterAbs;
  res.chestHeightAbs = chestHeightAbs;
  // tilt_B = 摄像头指向胸口点的俯仰角 (负=下俯), 不再使用固定角偏移
  res.tilt_B = -atan2(cfg.camHeightMm - chestHeightAbs, D_cam) * RAD2DEG;
  res.tilt_B = constrain(res.tilt_B, -45.0f, 45.0f);

  // ---- Step 6: 灯云台A 角度（灯光照射） ----
  // Δx_lamp = ΔX + (rearPosition - xAnchor)  视差修正
  float deltaXLamp = res.deltaX + (float)(res.rearPosition - xAnchor);
  // pan_A = atan2(Δx_lamp, D_lamp) × 180/π  自适应距离
  // 2026-08-19 修改: pan_A 取反。实测追踪时射灯云台左右转向与人运动方向相反
  // (手动方向键正常、仅追踪反), 说明视觉端 pan 符号与灯端舵机转向约定相反,
  // 在计算源头取反修正。两处同步改: 本处 + computeLampPTZ()。
  res.pan_A = -atan2(deltaXLamp, D_lamp) * RAD2DEG;
  res.pan_A = constrain(res.pan_A, -90.0f, 90.0f);
  // tilt_A 高度差换算: 灯离地 = camHeightMm + railHeightDiff (灯更高 → 更下俯)
  // 胸口相对摄像头高度 = D_cam × tan(tilt_B) = chestHeightAbs - camHeightMm
  float chestRelCam  = D_cam * tan(res.tilt_B / RAD2DEG);
  float chestRelLamp = chestRelCam - cfg.railHeightDiff;
  res.tilt_A = atan2(chestRelLamp, D_lamp) * RAD2DEG;
  // 2026-08-19 新增: +tiltOffsetDeg 仰角偏移(默认+30°), 补偿灯端机械零位偏差
  // (实测云台仰角偏低、光斑照偏下, 用户要求直接加 30° 抬高)
  res.tilt_A = constrain(res.tilt_A + cfg.tiltOffsetDeg, -45.0f, 45.0f);

  res.valid = true;
  return res;
}

// ===================== 灯云台 PTZ 独立计算 =====================
void computeLampPTZ(int xAnchor, const PersonTrackResult& trackResult,
                    float& pan_A, float& tilt_A) {
  // §4.8: Δx_lamp = ΔX + (rearPosition - xAnchor)
  float deltaXLamp = trackResult.deltaX + (float)(trackResult.rearPosition - xAnchor);

  // 灯靠客户侧，使用自适应灯深度 distLamp（替代固定 yPerson）
  // pan_A = atan2(Δx_lamp, D_lamp) × 180/π
  float D_lamp = trackResult.distLamp > 0.0f ? trackResult.distLamp : cfg.yPerson;
  // 2026-08-19 修改: pan_A 取反 (与 computeTracking Step 6 同步, 原因见该处注释)
  pan_A = -atan2(deltaXLamp, D_lamp) * RAD2DEG;
  pan_A = constrain(pan_A, -90.0f, 90.0f);

  // tilt_A 高度差修正（与 computeTracking Step 6 同式）
  float D_cam = trackResult.distCam > 0.0f ? trackResult.distCam : (cfg.yPerson + cfg.dRails);
  float chestRelCam  = D_cam * tan(trackResult.tilt_B / RAD2DEG);
  float chestRelLamp = chestRelCam - cfg.railHeightDiff;
  tilt_A = atan2(chestRelLamp, D_lamp) * RAD2DEG;
  // 2026-08-19 新增: +tiltOffsetDeg 仰角偏移 (与 computeTracking Step 6 同步修改)
  tilt_A = constrain(tilt_A + cfg.tiltOffsetDeg, -45.0f, 45.0f);
}

// ===================== 重置 height 平滑状态 =====================
// 无云台B: 不再有 deadzone/confirm 防抖状态; 仅重置 height EMA 平滑,
// 确保新一轮追踪从干净状态开始。函数名保留以兼容多处调用方。
void resetTrackingDebounce() {
  hPxSmoothed = 0.0f;
  hPxLast     = 0.0f;
  hPxInit     = false;
}
