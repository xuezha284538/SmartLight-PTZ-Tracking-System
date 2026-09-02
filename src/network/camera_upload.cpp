#include "network/camera_upload.h"
#include "network/http_reporter.h"

// ===================== URL 编码 (查询参数安全) =====================
static String urlEncode(const String& str) {
  String encoded;
  encoded.reserve(str.length() * 3);
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      char buf[4];
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

// ===================== 通过 HTTP 向 ESP32-S3-CAM 发送命令 =====================
// 8266 --(HTTP GET /cmd?c=<cmd>)--> ESP32-S3-CAM
// 替代原 Serial2 串口通信, 无需物理接线
bool sendCamCmd(const String& cmd) {
  if (cmd.length() == 0) return false;

  WiFiClient client;
  HTTPClient http;
  String url = "http://" + String(ESP32_CAM_HOST) + ":" + String(ESP32_CAM_PORT) +
               "/cmd?c=" + urlEncode(cmd);

  http.begin(client, url);
  http.setTimeout(3000);
  int httpCode = http.GET();
  http.end();

  if (httpCode > 0) {
    DEBUG_SERIAL.printf("[CAM-CMD] %s -> %d\n", cmd.substring(0, 20).c_str(), httpCode);
  } else {
    DEBUG_SERIAL.printf("[CAM-CMD] %s 失败: %s\n", cmd.substring(0, 20).c_str(),
                        http.errorToString(httpCode).c_str());
  }
  return httpCode > 0;
}

// ===================== 触发 ESP32-CAM 拍照并上传 =====================
// 通过 HTTP 向 ESP32-S3-CAM 的 /cmd 接口依次发送:
//   U<url>  - 设置上传 URL
//   K<token>- 设置上传 token
//   C       - 触发拍照并上传
bool triggerEsp32CamCapture(const String& uploadUrl, const String& uploadToken) {
  // 构建完整上传 URL: http://<serverHost>:<httpPort><uploadUrl>
  String fullUrl = httpUrl(uploadUrl);

  // 1. 设置上传 URL (U + 完整 URL)
  if (fullUrl.length() > 0) {
    sendCamCmd("U" + fullUrl);
  }

  // 2. 设置上传 token (K + token)
  if (uploadToken.length() > 0) {
    sendCamCmd("K" + uploadToken);
  }

  // 3. 触发拍照 (C = Capture)
  sendCamCmd("C");

  DEBUG_SERIAL.printf("[CAM-UPLOAD] triggered capture via HTTP, uploadUrl=%s\n", fullUrl.c_str());
  return true;
}

// ===================== HTTP multipart/form-data 上传 JPEG =====================
// 注意: 此函数用于 8266 直接上传 JPEG 到后端的场景 (预留)
// 当前正常流程由 ESP32-S3-CAM 通过 HTTP 自行上传
int uploadPhotoToBackend(const String& uploadUrl, const String& uploadToken,
                         const uint8_t* jpegData, size_t jpegLen) {
  if (!jpegData || jpegLen == 0) {
    DEBUG_SERIAL.println("[CAM-UPLOAD] no JPEG data");
    return -1;
  }

  // ESP8266 堆内存有限, 仅支持小图片 (< 8KB body 开销)
  // JPEG > 30KB 时建议由 ESP32-CAM 直接上传
  if (jpegLen > 30000) {
    DEBUG_SERIAL.printf("[CAM-UPLOAD] JPEG too large for 8266: %u bytes\n", (unsigned)jpegLen);
    return -2;
  }

  // 构建完整 URL (含 token 查询参数)
  String url = httpUrl(uploadUrl);
  if (uploadToken.length() > 0) {
    url += "?token=" + uploadToken;
  }

  WiFiClient client;
  HTTPClient http;
  http.begin(client, url);
  http.setTimeout(5000);

  // 构建 multipart/form-data 边界
  String boundary = "----TraeBoundary" + String(millis());
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);

  // 构建 body 头部和尾部
  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"file\"; filename=\"capture.jpg\"\r\n"
                "Content-Type: image/jpeg\r\n\r\n";
  String tail = "\r\n--" + boundary + "--\r\n";

  size_t headLen = head.length();
  size_t tailLen = tail.length();
  size_t totalLen = headLen + jpegLen + tailLen;

  // 分配完整 body 缓冲区
  uint8_t* body = (uint8_t*)malloc(totalLen);
  if (!body) {
    DEBUG_SERIAL.println("[CAM-UPLOAD] malloc failed");
    http.end();
    return -3;
  }

  // 拼接 body: head + JPEG + tail
  memcpy(body, head.c_str(), headLen);
  memcpy(body + headLen, jpegData, jpegLen);
  memcpy(body + headLen + jpegLen, tail.c_str(), tailLen);

  int httpCode = http.POST(body, totalLen);
  free(body);

  if (httpCode > 0) {
    DEBUG_SERIAL.printf("[CAM-UPLOAD] upload success: %d (%u bytes)\n", httpCode, (unsigned)jpegLen);
  } else {
    DEBUG_SERIAL.printf("[CAM-UPLOAD] upload failed: %d\n", httpCode);
  }

  http.end();
  return httpCode;
}
