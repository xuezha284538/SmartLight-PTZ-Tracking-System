#pragma once

// mDNS 主机名发现
// 设备启动后可通过 <deviceId>.local 访问，不依赖 IP 变化
void beginMDNS();
void updateMDNS();
