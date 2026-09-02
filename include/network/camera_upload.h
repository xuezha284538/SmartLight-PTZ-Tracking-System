#pragma once
#include "app_config.h"

// 通过 HTTP 向 ESP32-S3-CAM 发送单条命令 (U/K/C/Z/T/S)
// cmd: 完整命令字符串, 如 "Uhttp://..." / "Ktoken" / "C" / "Zcenter"
// 返回 true 表示 HTTP 请求成功
bool sendCamCmd(const String& cmd);

// 触发 ESP32-CAM 执行一次拍照并上传
// 通过 HTTP 向 ESP32-S3-CAM 的 /cmd 接口发送 U/K/C 命令序列
// uploadUrl:  相对路径 (如 /device/cam/capture-task/xxx/photo)
// uploadToken: 一次性 token
// 返回 true 表示命令已发送
bool triggerEsp32CamCapture(const String& uploadUrl, const String& uploadToken);

// 通过 HTTP multipart/form-data 上传 JPEG 到后端
// uploadUrl:  相对路径
// uploadToken: 一次性 token
// jpegData:    JPEG 二进制数据指针
// jpegLen:      数据长度
// 返回 HTTP 状态码 (>0 成功, <0 失败)
int uploadPhotoToBackend(const String& uploadUrl, const String& uploadToken,
                         const uint8_t* jpegData, size_t jpegLen);
