#pragma once
#include "app_config.h"

// HUSKYLENS I2C 库
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include <HUSKYLENS.h>
#pragma GCC diagnostic pop

// ===================== 初始化 =====================
void initPersonTracker();
bool isHuskyLensReady();

// ===================== 算法模式应用 =====================
// 按 cfg.trackAlgorithm 切换 HUSKYLENS 算法:
//   0 = 人脸识别 (识别到人自动触发, 无需按键)
//   1 = 物体追踪 (需按键学习目标, wiki 7.2 旧行为)
// 返回是否写入成功 (老固件可能不支持 writeAlgorithm 命令)
bool applyHuskyAlgorithm();

// ===================== 读取检测框 =====================
// 每帧调用，返回 true 表示检测到人，结果写入 out
bool readPersonFromHuskyLens(HUSKYLENSResult& out);

// ===================== 坐标转换（核心引擎） =====================
// §4 六步坐标转换: 图像系(px) → 物理系(mm) → 云台系(°)
// xAnchor: 活跃灯节点在前滑轨的安装位置 (mm)
PersonTrackResult computeTracking(const HUSKYLENSResult& r, int xAnchor);

// ===================== 灯云台 PTZ 独立计算 =====================
// 从已有追踪结果计算灯云台 Pan/Tilt
void computeLampPTZ(int xAnchor, const PersonTrackResult& trackResult,
                    float& pan_A, float& tilt_A);

// ===================== 重置 height 平滑状态 =====================
// 无云台B: 不再有 deadzone/confirm 防抖; 仅重置 height EMA 平滑状态。
// 函数名保留以兼容多处调用方。
void resetTrackingDebounce();
