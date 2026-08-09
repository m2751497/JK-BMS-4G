/*
 * ESP32-S3 JK-BMS 4G 远程监控网关固件（合并版 v2.15）
 *
 * 功能:
 *   1. BLE 客户端: 连接极空 BMS, 特征选择"属性驱动 + handle 优先"(FIX-19/v9.3):
 *      V20S 双 FFE1(0x03 写 / 0x05 通知) 与 V17/V18/V19 单 FFE1(写+通知一体) 及其他
 *      handle 布局全兼容, 不再硬编码 0x03/0x05; 订阅按特征能力选 notify/indicate(FIX-20)
 *   2. BLE 中继服务: 中继完整版 v9.43 实现 —— 多客户端 (MAX_BLE_CLIENTS=5) + 帧级转发
 *      (重组完整帧 + CRC 校验通过后才按各客户端 MTU 分块推送, 150B 块模拟真实 BMS) +
 *      双通道 (数据帧帧级干净转发 / AT 心跳与命令帧回显原样透传, 解决 App 6 秒断开 reason=531);
 *      服务结构对齐真实 BMS: FFE0(FFE1/FFE2/FFE3) + FF10(FF11/FF12);
 *      广播名使用中继名称 g_config.relayName (Web 后台可设、默认 "JK-RELAY", UTF-8 安全截断);
 *      BMS 未连接时中继隐身 (停广播+断开旧客户端, 防抢连挤 MTU 协商) v9.41/42
 *   3. Web 管理后台: WiFi AP 模式 (192.168.4.1), HTTP REST API + WebSocket(81) 实时推送,
 *      Preferences 配置持久化 (BMS MAC / AP 账号密码 / 中继名称); WiFi 常开 (v9.33 不自动关闭)
 *   4. WiFi 射频策略: 低发射功率 8.5dBm + beacon 100ms (AP 响应优先)
 *   5. 4G/GPS 串口: UART1 (TX=13, RX=12) 接银尔达 M100PG-DTU, 支持指令 chaxun / getcsq /
 *      getgps / getblestatus, GPS 查询指令 config,get,gpsext, 输出 gps:fix,lonDir,lon,latDir,lat,speed
 *   6. 双模式定时上报 (核心新增): 解析 UART1 下发的 mode,realtime/track,<bms_ms>,<gps_ms> 指令,
 *      track 默认 GPS 30s (v2.6); 静止省流 (v2.7, 全部模式生效): 速度≥3km/h 且位移≥30m
 *      才算运动(双条件防 GPS 漂移误判), 静止≥60s 停发 GPS, 移动自动 2s 实时上报;
 *      BMS 双重判断(v2.12): 网页开→一律 2s 实时(充/放电/停放); 网页关→一律 30s;
 *      GPS 只看移动/静止(v2.11, 网页开关无关): 移动 2s 实时 / 静止≥60s 停发, 30s 本地探测;
 *      BMS 充放电驱动(v2.9): |I|≥0.3A 判充/放电;
 *      电流/电压解析按串数动态偏移(24S:126 / 32S:158, 修复 24 串读错位置 bug);
 *      移动自动恢复 2s 实时上报 (网页关也实时), 打开网页(realtime 指令)始终 2s;
 *      按 BMS/GPS 间隔 (毫秒) 定时上报 BMS 原始帧 b:r,<hex> 与 GPS 文本, millis() 节流不阻塞
 *
 * 来源 (v2.15 起):
 *   - BLE 客户端 + BLE 中继 + Web 后台: 中继完整版 v9.43 (修复版, S3/C3 通用) ——
 *     BMS 读取与中继以完整版为准 (帧级转发 / 双通道 / 多客户端 / FIX-1~27,
 *     含 v9.13 AT心跳保活、v9.14 连接参数、v9.33 扫描退避/WiFi常开、v9.38 命令缓存补发、
 *     v9.40/43 MTU降级、v9.41/42 隐身广播、v9.17 手动扫描)
 *   - 4G/GPS 固件 (UART1 串口初始化 / 指令处理 / GPS 解析 / 双模式上报): 参考代码2\极空保护板-蓝牙4G方案\esp32s3\esp32s3.ino
 *   合并规则: BLE 部分以 中继完整版.txt 为准, esp32s3.ino 仅并入 4G/GPS 串口与指令处理部分;
 *   notifyCB 在完整版基础上保留 lastRawFrame 保存 (供 b:r,<hex> 4G 上报)
 *
 * 所需库 (Arduino IDE 库管理器安装):
 *   1. Arduino-ESP32 Core >= 3.0.0
 *      板管理器 URL: https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
 *   2. NimBLE-Arduino >= 2.2.2 (by h2zero) —— 必须, 勿用 Arduino 自带 Bluedroid
 *      (双 FFE1 同 UUID 需 NimBLE 的 vector 同时存两个特征, Bluedroid 的 map 会覆盖丢失)
 *   3. WebSockets (by Markus Sattler)
 *   4. ArduinoJson
 *
 * 接线 (ESP32-S3 UART1 与 M100PG-DTU 对接):
 *   GPIO12 (UART1 RX)  ← DTU TX
 *   GPIO13 (UART1 TX)  → DTU RX
 *   GND                → GND (共地)
 *
 * 串口协议 (115200 8N1):
 *   上行 (ESP32 → DTU → MQTT T 主题):
 *     b:r,<hex帧>                       BMS 原始帧 (首字节无 0x 前缀的 hex 串, 服务端 parser.php 解析)
 *     gps:<fix>,<lonDir>,<lon>,<latDir>,<lat>,<speed>   GPS 定位 (speed 单位 km/h)
 *     blestatus:<state>,<mac>,<0|1>     蓝牙连接状态
 *     csq:<val>                         4G 信号强度
 *   下行 (MQTT R 主题 → DTU → ESP32):
 *     mode,realtime,<bms_ms>,<gps_ms>   实时模式 (网页打开, 高频上报)
 *     mode,track,<bms_ms>,<gps_ms>      追踪模式 (网页关闭, 低频上报)
 *     chaxun / getcsq / getgps / getblestatus
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <string>
#include "esp_task_wdt.h"   // 自愈: 任务看门狗 (ESP32 卡死自动重启)

// ==================== 配置 ====================
// BMS 蓝牙 MAC 地址 (默认值, 可通过 Web 后台修改)
const char* DEFAULT_BMS_MAC = "00:00:00:00:00:00";
// HTTP 端口
const uint16_t HTTP_PORT = 80;
// WebSocket 端口
const uint16_t WS_PORT = 81;

// BLE 服务与特征 UUID (对齐中继完整版 v8.1: 真实 BMS 结构 FFE0 服务 + FF10 服务)
const NimBLEUUID SERVICE_UUID((uint16_t)0xFFE0);
const NimBLEUUID CHAR_UUID((uint16_t)0xFFE1);
const NimBLEUUID VENDOR_SERVICE_UUID((uint16_t)0xFF10);   // 真实 BMS 的 0xFF10 服务 (原 0xE07F 不存在)
const NimBLEUUID CHAR_FFE2_UUID((uint16_t)0xFFE2);
const NimBLEUUID CHAR_FFE3_UUID((uint16_t)0xFFE3);
const NimBLEUUID CHAR_FF11_UUID((uint16_t)0xFF11);
const NimBLEUUID CHAR_FF12_UUID((uint16_t)0xFF12);

// JK-BMS 命令
const uint8_t CMD_CELL_INFO   = 0x96;
const uint8_t CMD_DEVICE_INFO = 0x97;

// 请求间隔 (毫秒): 无订阅客户端时 5s 轮询保持 Web 后台/4G 数据新鲜
const unsigned long REQUEST_INTERVAL = 5000;
// ★FIX-2: 有订阅客户端时的低频兜底轮询间隔 (让客户端驱动请求, 避免 ESP32 轮询与 App 请求响应交错)
const unsigned long IDLE_POLL_INTERVAL = 30000;
// 无数据超时重连 (毫秒)
const unsigned long DATA_TIMEOUT = 30000;
// 重连间隔 (毫秒)
const unsigned long RECONNECT_INTERVAL = 3000;

// JK 帧长度 (完整版抓包验证: cmd=0x01/0x02/0x03 全部 300B)
const size_t JK_FRAME_LEN = 300;

// ==================== 全局变量 ====================

// BLE 相关 (客户端: 连接真实 BMS)
NimBLEClient* pClient = nullptr;
NimBLERemoteCharacteristic* pWriteChar = nullptr;
NimBLERemoteCharacteristic* pNotifyChar = nullptr;
const NimBLEAdvertisedDevice* g_advDevice = nullptr;
bool g_doConnect = false;

// BLE 相关 (服务端: 模拟 BMS, 供极空 App 连接)
NimBLEServer* pBleServer = nullptr;
NimBLECharacteristic* pBleServerChar = nullptr;
NimBLEService* pBleService = nullptr;
NimBLEService* pVendorService = nullptr;
NimBLECharacteristic* pVendorChar = nullptr;
// 多客户端 (中继完整版 v8.1): 最大连接数 (sdkconfig CONFIG_BT_NIMBLE_MAX_CONNECTIONS=6, 1 个留给 BMS 客户端)
#define MAX_BLE_CLIENTS 5
// 多客户端: 每连接订阅跟踪 (connHandle -> subscribed)
struct ConnSubState {
  uint16_t connHandle;
  bool subscribed;
  bool active;
};
ConnSubState g_connStates[MAX_BLE_CLIENTS];
volatile int g_subscribedConnCount = 0;  // v9.34: volatile (onSubscribe 在 NimBLE 任务写, loop 读)
volatile int g_bleClientCount = 0;  // v9.33: 自维护 BLE 服务端连接计数 (回调链安全, 替代 getConnectedCount)

// 原始帧缓存 (订阅后推送完整帧给 App, 从帧头 55AAEB90 开始, 保证接收端拼装正确)
uint8_t g_cellInfoFrame[300];
size_t g_cellInfoLen = 0;
bool g_hasCellInfo = false;
uint8_t g_deviceInfoFrame[300];
size_t g_deviceInfoLen = 0;
bool g_hasDeviceInfo = false;

// v9.38: BMS 未连接期间缓存的 App 命令 (显示屏/极空 App 先连上中继就发 0x97/0x96,
//   BMS 还没连上时原逻辑直接丢弃 → App 收不到响应; 缓存最近一条, BMS 连上后补发)
uint8_t g_pendingCmd[20] = {0};
int g_pendingCmdLen = 0;

// v9.40/v9.43: MTU 低时重试计数 (防"MTU<100 无限断开重试导致永远连不上")
#define MTU_MAX_RETRY 2
int g_mtuRetryCount = 0;

// v9.17: 手动扫描收集 (Web 后台"扫描保护板")
struct BmsScanEntry {
  char mac[20];
  char name[33];
  int rssi;
};
#define MAX_SCAN_ENTRIES 20
BmsScanEntry g_scanList[MAX_SCAN_ENTRIES];
volatile int g_scanCount = 0;
volatile bool g_manualScanning = false;
volatile bool g_scanStarting = false;  // v9.33: 扫描排队标志 (HTTP handler 只设标志)
volatile int g_bmsScanFailCount = 0;   // v9.33: 自动扫描连续失败次数 (指数退避)

// Web 服务器
WebServer httpServer(HTTP_PORT);
WebSocketsServer webSocketServer = WebSocketsServer(WS_PORT);

// Preferences 存储
Preferences prefs;

// 数据缓存
struct BMSData {
  bool valid = false;
  float totalVoltage = 0;
  float current = 0;
  float minCellVoltage = 0;
  float maxCellVoltage = 0;
  float avgCellVoltage = 0;
  float deltaCellVoltage = 0;
  uint8_t soc = 0;
  float capacityRemaining = 0;
  float cycleCapacity = 0;
  uint32_t cycleCount = 0;
  float mosfetTemp = 0;
  float temp1 = 0;
  float temp2 = 0;
  uint8_t cellCount = 0;
  float cellVoltages[32] = {0};
  unsigned long lastUpdate = 0;
  char version[16] = {0};
  int rssi = 0;
};

BMSData g_bmsData;

// 配置
struct GatewayConfig {
  char relayName[32] = "JK-RELAY";
  char bmsMac[20] = "00:00:00:00:00:00";
  char apSsid[32] = "JK-BMS";
  char apPassword[64] = "12345678";
};

GatewayConfig g_config;

// 状态
std::vector<uint8_t> frameBuffer;
bool bmsConnected = false;
unsigned long lastRequest = 0;
unsigned long lastDataTime = 0;
unsigned long lastReconnectAttempt = 0;
bool configDirty = false;
bool g_wifiEnabled = true;  // WiFi 开关状态

// WiFi 自动关闭计时 (10分钟无操作后自动关闭)
const unsigned long WIFI_AUTO_OFF_MS = 10UL * 60UL * 1000UL;  // 10分钟
unsigned long g_lastWebRequest = 0;   // 最后一次 Web 请求时间

// BOOT 按键 (GPIO0) 长按检测
const int BOOT_BTN_PIN = 0;
unsigned long g_btnPressStart = 0;
bool g_btnPressed = false;

// ==================== 4G/GPS 串口与双模式上报 (合并自 esp32s3.ino) ====================

// UART1 接银尔达 M100PG-DTU (TX=13 → DTU RXD, RX=12 ← DTU TXD)
const int SERIAL1_TX_PIN = 13;
const int SERIAL1_RX_PIN = 12;
HardwareSerial SERIAL1(1);

// 串口行接收缓冲 (行协议, \r\n 结尾)
String uart2ForwardBuffer = "";
unsigned long lastUartRxTime = 0;

// 最近一帧完整 BMS 原始帧 (由 notifyCB 捕获, 供 b:r,<hex> 上报)
uint8_t lastRawFrame[300];
uint16_t lastRawFrameLen = 0;

// 双模式定时上报 (默认 track, 由服务端 mode,realtime/track,<bms_ms>,<gps_ms> 指令切换)
String g_reportMode = "track";
unsigned long g_realtimeBmsMs = 2000;    // realtime: BMS 2s (网页开, 充/放电时)
unsigned long g_realtimeGpsMs = 2000;    // realtime: GPS 2s
unsigned long g_trackBmsMs = 30000;      // track: BMS 30s (网页关/停放)
unsigned long g_trackGpsMs = 30000;      // track: GPS 30s (静止探测周期)

// ===== 静止省流 (v2.7): 静止/运动判断在全部模式下生效(网页开/关都按此) =====
const float G_GPS_MOVING_KMH = 3.0f;          // 速度 ≥3km/h 视为移动
const float G_GPS_MOVE_DIST_M = 30.0f;        // 位移 ≥30m 视为移动 (防 GPS 漂移误判: 漂移一般 <30m, 若实际漂移更大可调大此值)
const unsigned long G_STILL_STOP_MS = 60000UL; // 连续静止 60s 后停发 GPS
bool g_moving = false;                         // 上次 GPS 判断: 是否移动
unsigned long g_stillStart = 0;                // 连续静止开始时间 (0=移动中)
float g_lastLat = 0.0f, g_lastLon = 0.0f;      // 上次 GPS 上报点 (位移计算基准)
unsigned long g_bmsIntervalMs = g_trackBmsMs;   // 当前生效的 BMS 上报间隔
unsigned long g_gpsIntervalMs = g_trackGpsMs;   // 当前生效的 GPS 上报间隔
unsigned long g_lastBmsReport = 0;       // 上次 BMS 上报时间 (millis)
unsigned long g_lastGpsReport = 0;       // 上次 GPS 查询时间 (millis)
bool g_gpsQueryPending = false;          // GPS 查询挂起 (防连续下发, 等 DTU 响应或超时)
unsigned long g_gpsQueryTime = 0;
const float G_CURRENT_THRESHOLD_A = 0.3f; // 充放电电流阈值: |I|≥0.3A 视为充/放电(上报 BMS), 停放停报 (v2.9)
unsigned long g_serialTxTime = 0;        // 串口发送保护: 上次发送完成时间 (防多条数据粘连)
String g_pendingTx = "";                 // 待发送队列 (间隔不足时排队, loop 中稍后发送, 非阻塞)

// ===== 自愈机制 (v2.4): 三层兜底, 免人工断电 =====
// 1) 任务看门狗: ESP32 卡死(死循环/阻塞) 30s 自动重启
// 2) DTU 串口无响应: 任何串口行都代表 DTU 活着; 超过 8 分钟无响应 → 重启
// 3) MQTT 下行无响应: 收到 mode,/chaxun/getcsq 等 EMQX 下行指令 = 上下行链路通;
//    超过 12 分钟无下行(服务端 cron 每 30s 强制下发) → 重启 (覆盖 DTU MQTT 静默断开场景)
unsigned long lastDtuResponse = 0;       // 上次收到 DTU 串口响应 (任何行)
unsigned long lastDownlink = 0;          // 上次收到 EMQX 下行指令
const unsigned long DTU_RESPONSE_TIMEOUT = 8UL * 60 * 1000;   // DTU 无响应 8 分钟
const unsigned long DOWNLINK_TIMEOUT    = 12UL * 60 * 1000;   // 无下行 12 分钟
const unsigned long STARTUP_GRACE       = 2UL * 60 * 1000;    // 启动 2 分钟宽限 (避免开机即重启)
// DTU 电源控制引脚 (冷重启: 断电 3 秒再上电, 需外接 MOS/继电器控制 DTU 电源; -1 = 不启用)
const int DTU_POWER_PIN = -1;
// ★ 定时自动重启 (电摩场景无插座, 无法远程断电): 连续运行满 N 小时自动重启一次,
//   清理长时间运行积累的偶发故障 (不依赖下行链路); 0 = 禁用
const int AUTO_REBOOT_HOURS = 24;

// ==================== 前向声明 ====================
void pushBMSData();
// 完整版 v8.1: notifyCB / 帧级转发 前向声明 (缺声明会编译报错)
void notifyCB(NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify);
void forwardFrameToClients(const uint8_t* frame, size_t len);
void forwardFrameToClient(uint16_t connHandle, const uint8_t* frame, size_t len);
void initWiFiAP();
void initWebServer();
void initWebSocket();

// ★ 4G/GPS 串口模块 (合并自 esp32s3.ino + 新增双模式定时上报)
void handle4G();
void sendSerial1(const String& s);   // 串口发送保护 (防多条数据粘连)
void handleSerial1Receive();
void handleSerial1Command(String data);
void handleModeCommand(const String& clean);
void sendRawFrameToSERIAL1();
String bleStateStr();

// ==================== 工具函数 ====================

uint8_t jkCRC(const uint8_t* data, uint16_t len) {
  uint8_t crc = 0;
  for (uint16_t i = 0; i < len; i++) {
    crc += data[i];
  }
  return crc;
}

// ★FIX-6 (完整版): 在 buffer 中查找下一个 JK 帧头 (55 AA EB 90), 返回偏移; 找不到返回 (size_t)-1
size_t findFrameHeader(const std::vector<uint8_t>& buf, size_t from = 0) {
  if (buf.size() < 4) return (size_t)-1;
  for (size_t i = from; i + 4 <= buf.size(); i++) {
    if (buf[i] == 0x55 && buf[i + 1] == 0xAA &&
        buf[i + 2] == 0xEB && buf[i + 3] == 0x90) {
      return i;
    }
  }
  return (size_t)-1;
}

uint16_t get16(const uint8_t* d, size_t i) {
  return (uint16_t(d[i + 1]) << 8) | uint16_t(d[i]);
}

uint32_t get32(const uint8_t* d, size_t i) {
  return (uint32_t(get16(d, i + 2)) << 16) | get16(d, i);
}

void sendCommand(uint8_t cmd) {
  if (!pWriteChar || !bmsConnected) {
    return;
  }
  uint8_t frame[20] = {0};
  frame[0] = 0xAA;
  frame[1] = 0x55;
  frame[2] = 0x90;
  frame[3] = 0xEB;
  frame[4] = cmd;
  frame[5] = 0x00;
  frame[19] = jkCRC(frame, 19);

  pWriteChar->writeValue(frame, 20, false);
}

// ==================== 数据解析 ====================

void parseCellInfo(const uint8_t* data, size_t len) {
  // v2.9: 24S/32S 字段偏移不同 (与 parser.php 一致): 24S 从 0 偏移, 32S 从 32 偏移。
  //   之前固定 off=32 导致 24 串电池读错位置 (电流/电压/温度全错位)。

  float minCellV = 100.0f;
  float maxCellV = -100.0f;
  float avgCellV = 0.0f;
  uint8_t cellsEnabled = 0;

  for (uint8_t i = 0; i < 32; i++) {
    float v = get16(data, i * 2 + 6) * 0.001f;
    g_bmsData.cellVoltages[i] = v;
    if (v > 0) {
      avgCellV += v;
      cellsEnabled++;
      if (v < minCellV) minCellV = v;
      if (v > maxCellV) maxCellV = v;
    }
  }

  if (cellsEnabled > 0) {
    avgCellV /= cellsEnabled;
    g_bmsData.cellCount = cellsEnabled;
  }
  // 根据激活串数选偏移: >24 串 → 32S 布局
  size_t off2 = (cellsEnabled > 24) ? 32 : 0;
  if (maxCellV < 0) maxCellV = 0;
  if (minCellV > 100) minCellV = 0;

  g_bmsData.minCellVoltage = minCellV;
  g_bmsData.maxCellVoltage = maxCellV;
  g_bmsData.avgCellVoltage = avgCellV;
  g_bmsData.deltaCellVoltage = maxCellV - minCellV;
  g_bmsData.mosfetTemp = (float)((int16_t)get16(data, 112 + off2)) * 0.1f;
  g_bmsData.totalVoltage = get32(data, 118 + off2) * 0.001f;
  g_bmsData.current = (float)((int32_t)get32(data, 126 + off2)) * 0.001f;
  g_bmsData.temp1 = (float)((int16_t)get16(data, 130 + off2)) * 0.1f;
  g_bmsData.temp2 = (float)((int16_t)get16(data, 132 + off2)) * 0.1f;
  g_bmsData.soc = data[141 + off2];
  g_bmsData.capacityRemaining = get32(data, 142 + off2) * 0.001f;
  g_bmsData.cycleCount = get32(data, 150 + off2);
  g_bmsData.cycleCapacity = get32(data, 154 + off2) * 0.001f;
  g_bmsData.valid = true;
  g_bmsData.lastUpdate = millis();

  // 更新 RSSI (如果已连接)
  if (pClient && bmsConnected) {
    g_bmsData.rssi = pClient->getRssi();
  }

  // 数据日志由 Web 后台查看, 串口不打印 (避免刷屏)

  // 推送 WebSocket
  pushBMSData();
}

void parseDeviceInfo(const uint8_t* data, size_t len) {
  // ★FIX-10 (完整版): 0x97 响应 (cmd=0x03) 是 ASCII 文本帧 (抓包确认):
  //   offset  6: 型号 "JK-BD6A24S12PD"
  //   offset 26: 固件版本 "20.25"
  //   offset 43: 电池名称 "cy-90ah"
  //   offset 68: 密码 "1234"
  if (len >= 33) {
    bool ok = true;
    char ver[8] = {0};
    for (int i = 0; i < 7; i++) {
      char c = (char)data[26 + i];
      if (c < 0x20 || c > 0x7E) { ok = false; break; }  // 要求可打印 ASCII
      ver[i] = c;
    }
    if (ok) {
      snprintf(g_bmsData.version, sizeof(g_bmsData.version), "%s", ver);
      Serial.printf("[DIAG] 设备信息帧: 版本=%s (0x03 链路正常)\n", g_bmsData.version);
      return;
    }
  }

  // 回退: 旧协议数字版本 (offset 6/8 为硬件/固件版本)
  if (len >= 10) {
    uint16_t hwVer = get16(data, 6);
    uint16_t fwVer = get16(data, 8);
    if (fwVer > 0) {
      snprintf(g_bmsData.version, sizeof(g_bmsData.version), "%u.%u.%u",
        (fwVer >> 8) & 0xFF, (fwVer >> 4) & 0x0F, fwVer & 0x0F);
    } else if (hwVer > 0) {
      snprintf(g_bmsData.version, sizeof(g_bmsData.version), "%u.%u.%u",
        (hwVer >> 8) & 0xFF, (hwVer >> 4) & 0x0F, hwVer & 0x0F);
    }
  }
}

// ==================== JSON 序列化 ====================

String buildBMSJson() {
  StaticJsonDocument<1024> doc;

  doc["valid"] = g_bmsData.valid;
  doc["totalVoltage"] = g_bmsData.totalVoltage;
  doc["current"] = g_bmsData.current;
  doc["minCellVoltage"] = g_bmsData.minCellVoltage;
  doc["maxCellVoltage"] = g_bmsData.maxCellVoltage;
  doc["avgCellVoltage"] = g_bmsData.avgCellVoltage;
  doc["deltaCellVoltage"] = g_bmsData.deltaCellVoltage;
  doc["soc"] = g_bmsData.soc;
  doc["capacityRemaining"] = g_bmsData.capacityRemaining;
  doc["cycleCapacity"] = g_bmsData.cycleCapacity;
  doc["cycleCount"] = g_bmsData.cycleCount;
  doc["mosfetTemp"] = g_bmsData.mosfetTemp;
  doc["temp1"] = g_bmsData.temp1;
  doc["temp2"] = g_bmsData.temp2;
  doc["cellCount"] = g_bmsData.cellCount;
  doc["lastUpdate"] = g_bmsData.lastUpdate;
  doc["connected"] = bmsConnected;
  doc["mac"] = g_config.bmsMac;
  doc["version"] = g_bmsData.version;
  doc["rssi"] = g_bmsData.rssi;

  JsonArray cells = doc.createNestedArray("cellVoltages");
  for (uint8_t i = 0; i < g_bmsData.cellCount; i++) {
    cells.add(g_bmsData.cellVoltages[i]);
  }

  String output;
  serializeJson(doc, output);
  return output;
}

String buildStatusJson() {
  StaticJsonDocument<384> doc;

  doc["relayName"] = g_config.relayName;
  // BMS 连接状态 (ESP32 作为客户端连接真实保护板)
  doc["bmsConnected"] = bmsConnected;
  doc["bmsMac"] = g_config.bmsMac;
  doc["bmsRssi"] = g_bmsData.rssi;
  doc["bmsVersion"] = g_bmsData.version;
  doc["valid"] = g_bmsData.valid;
  doc["lastUpdate"] = g_bmsData.lastUpdate;
  doc["uptime"] = millis();
  // ★v2.14: 上次重启原因 (诊断)
  prefs.begin("gateway", true);
  doc["lastResetInfo"] = prefs.getString("lastResetInfo", "上次重启: --");
  prefs.end();
  // BLE 服务端 (ESP32 作为中继广播, 极空 App 连接到这里)
  uint32_t svcConn = g_bleClientCount;  // v9.33: 自维护计数 (HTTP 上下文安全, 统一方案)
  doc["bleServerConnected"] = (svcConn > 0);
  doc["bleServerConnCount"] = svcConn;
  doc["bleServerName"] = g_config.relayName;
  // WiFi 状态
  doc["wifiEnabled"] = g_wifiEnabled;
  doc["wifiStations"] = WiFi.softAPgetStationNum();
  // WebSocket 客户端
  doc["wsClients"] = webSocketServer.connectedClients();
  // 内存信息
  doc["heapTotal"] = ESP.getHeapSize();
  doc["heapFree"] = ESP.getFreeHeap();

  String output;
  serializeJson(doc, output);
  return output;
}

// ==================== WebSocket 推送 ====================

void pushBMSData() {
  if (webSocketServer.connectedClients() > 0) {
    String json = buildBMSJson();
    webSocketServer.broadcastTXT(json);
  }
}

// WebSocket 事件处理
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED: {
      Serial.printf("[WS] 客户端 %u 连接\n", num);
      // 发送初始数据
      String json = buildBMSJson();
      webSocketServer.sendTXT(num, json);
      break;
    }
    case WStype_DISCONNECTED:
      Serial.printf("[WS] 客户端 %u 断开\n", num);
      break;
    case WStype_TEXT: {
      // 处理客户端请求
      if (length > 0) {
        String msg((char*)payload);
        if (msg == "getStatus") {
          String status = buildStatusJson();
          webSocketServer.sendTXT(num, status);
        } else if (msg == "getData") {
          String data = buildBMSJson();
          webSocketServer.sendTXT(num, data);
        }
      }
      break;
    }
    default:
      break;
  }
}

// ==================== HTTP 路由 ====================

// 设置 CORS 头
void setCorsHeaders() {
  httpServer.sendHeader("Access-Control-Allow-Origin", "*");
  httpServer.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  httpServer.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  g_lastWebRequest = millis();  // 更新最后请求时间 (用于 WiFi 自动关闭)
}

// API: 获取 BMS 数据
void handleGetData() {
  setCorsHeaders();
  httpServer.sendHeader("Content-Type", "application/json");
  String json = buildBMSJson();
  httpServer.send(200, "application/json", json);
}

// ==================== OTA 固件升级 (v2.14, Web 网页上传) ====================
// 分区表 default_16MB 自带双 OTA 分区 (app0/app1 各 6.25MB), 上传新固件写另一分区,
// 成功后切换并重启; 失败自动回滚到旧固件。

// 上传分块回调
void handleOtaUpload() {
  HTTPUpload& up = httpServer.upload();
  if (up.status == UPLOAD_FILE_START) {
    Serial.printf("[OTA] 开始接收固件: %s, 大小=%u 字节\n", up.filename.c_str(), up.totalSize);
    if (up.totalSize > 0x600000) {   // OTA 分区约 6MB, 预留校验
      Serial.println("[OTA] 固件过大, 拒绝");
      Update.abort();
      return;
    }
    if (!Update.begin(up.totalSize)) {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.println("[OTA] 固件写入成功, 校验通过");
    } else {
      Update.printError(Serial);
    }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    Serial.println("[OTA] 上传中断");
  }
}

// 上传完成回调
void handleOtaDone() {
  if (Update.hasError()) {
    Serial.println("[OTA] 升级失败, 保持旧固件");
    httpServer.send(500, "application/json",
      "{\"success\":false,\"message\":\"升级失败, 已保持旧固件, 请重试\"}");
  } else {
    Serial.println("[OTA] 升级成功, 2 秒后重启...");
    httpServer.send(200, "application/json",
      "{\"success\":true,\"message\":\"升级成功, 设备重启中...\"}");
    httpServer.client().stop();
    delay(1500);
    ESP.restart();
  }
}

// API: 获取系统状态
void handleGetStatus() {
  setCorsHeaders();
  httpServer.sendHeader("Content-Type", "application/json");
  String json = buildStatusJson();
  httpServer.send(200, "application/json", json);
}

// API: 获取配置
void handleGetConfig() {
  setCorsHeaders();
  StaticJsonDocument<512> doc;
  doc["relayName"] = g_config.relayName;
  doc["bmsMac"] = g_config.bmsMac;
  doc["apSsid"] = g_config.apSsid;
  doc["apPassword"] = g_config.apPassword;
  doc["apIp"] = WiFi.softAPIP().toString();
  doc["wifiEnabled"] = g_wifiEnabled;
  doc["httpPort"] = HTTP_PORT;
  doc["wsPort"] = WS_PORT;

  String output;
  serializeJson(doc, output);
  httpServer.send(200, "application/json", output);
}

// API: 更新配置
void handlePostConfig() {
  setCorsHeaders();

  if (!httpServer.hasArg("plain")) {
    httpServer.send(400, "application/json", "{\"error\":\"缺少请求体\"}");
    return;
  }

  String body = httpServer.arg("plain");
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err) {
    httpServer.send(400, "application/json", "{\"error\":\"JSON 解析失败\"}");
    return;
  }

  if (doc.containsKey("relayName")) {
    String name = doc["relayName"].as<String>();
    if (name.length() > 0 && name.length() <= 31) {
      strncpy(g_config.relayName, name.c_str(), 31);
      g_config.relayName[31] = '\0';
      configDirty = true;
      Serial.printf("[Config] 中继名称已更新: %s (重启后生效)\n", g_config.relayName);
    }
  }

  if (doc.containsKey("bmsMac")) {
    String mac = doc["bmsMac"].as<String>();
    if (mac.length() > 0 && mac.length() <= 19) {
      strncpy(g_config.bmsMac, mac.c_str(), 19);
      g_config.bmsMac[19] = '\0';
      configDirty = true;
    }
  }

  if (doc.containsKey("apSsid")) {
    String ssid = doc["apSsid"].as<String>();
    if (ssid.length() >= 1 && ssid.length() <= 32) {
      strncpy(g_config.apSsid, ssid.c_str(), 31);
      g_config.apSsid[31] = '\0';
      configDirty = true;
    }
  }

  if (doc.containsKey("apPassword")) {
    String pwd = doc["apPassword"].as<String>();
    if (pwd.length() <= 63) {
      strncpy(g_config.apPassword, pwd.c_str(), 63);
      g_config.apPassword[63] = '\0';
      configDirty = true;
    }
  }

  // 保存到 NVS
  if (configDirty) {
    prefs.begin("gateway", false);
    prefs.putString("relayName", g_config.relayName);
    prefs.putString("bmsMac", g_config.bmsMac);
    prefs.putString("apSsid", g_config.apSsid);
    prefs.putString("apPassword", g_config.apPassword);
    prefs.end();
    configDirty = false;
  }

  StaticJsonDocument<512> resp;
  resp["success"] = true;
  resp["relayName"] = g_config.relayName;
  resp["bmsMac"] = g_config.bmsMac;
  resp["apSsid"] = g_config.apSsid;
  resp["apPassword"] = g_config.apPassword;

  String output;
  serializeJson(resp, output);
  httpServer.send(200, "application/json", output);
}

// API: 重启 BLE 连接
void handleReconnect() {
  setCorsHeaders();
  if (pClient) {
    pClient->disconnect();
  }
  bmsConnected = false;
  pWriteChar = nullptr;
  pNotifyChar = nullptr;
  g_advDevice = nullptr;
  g_doConnect = false;
  frameBuffer.clear();
  lastReconnectAttempt = 0;  // 允许立即重新扫描

  StaticJsonDocument<128> resp;
  resp["success"] = true;
  resp["message"] = "正在重新连接...";

  String output;
  serializeJson(resp, output);
  httpServer.send(200, "application/json", output);
}

// API: 触发数据请求
void handleRefresh() {
  setCorsHeaders();
  if (bmsConnected) {
    // 触发完整数据请求 (设备信息 + 电芯信息, 单连接版机制)
    sendCommand(CMD_DEVICE_INFO);
    sendCommand(CMD_CELL_INFO);
    lastRequest = millis();
    lastDataTime = millis();

    StaticJsonDocument<128> resp;
    resp["success"] = true;

    String output;
    serializeJson(resp, output);
    httpServer.send(200, "application/json", output);
  } else {
    httpServer.send(503, "application/json", "{\"error\":\"BMS 未连接\"}");
  }
}

// API: 触发 ESP32 重启
void handleReboot() {
  setCorsHeaders();
  httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"设备即将重启...\"}");
  delay(1000);
  ESP.restart();
}

// API: 关闭 WiFi (调试完成后关闭, 节省资源; BLE 继续运行)
void handleWifiOff() {
  setCorsHeaders();
  httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi 即将关闭, BLE 继续运行, 重启设备恢复 WiFi\"}");
  Serial.println("[WiFi] 收到关闭指令, 2 秒后关闭 WiFi...");

  // 延时确保响应已发送
  delay(2000);

  // 关闭 WebSocket 和 HTTP 服务器
  webSocketServer.close();
  httpServer.stop();

  // 关闭 WiFi AP
  WiFi.softAPdisconnect(true);
  g_wifiEnabled = false;

  Serial.println("[WiFi] 已关闭, 仅 BLE 运行 (重启设备可恢复 WiFi)");
}

// API: 开启 WiFi (重新启动 AP + 服务器)
void handleWifiOn() {
  setCorsHeaders();
  if (g_wifiEnabled) {
    httpServer.send(400, "application/json", "{\"success\":false,\"message\":\"WiFi 已在运行\"}");
    return;
  }

  // 先响应
  httpServer.send(200, "application/json", "{\"success\":true,\"message\":\"WiFi 即将开启...\"}");
  Serial.println("[WiFi] 收到开启指令, 正在启动 WiFi...");

  // 延时确保响应已发送
  delay(500);

  initWiFiAP();
  initWebServer();
  initWebSocket();
  g_wifiEnabled = true;
  g_lastWebRequest = millis();

  Serial.println("[WiFi] WiFi 已开启");
}

// ==================== BLE 扫描 ====================

// OPTIONS 预检请求
void handleOptions() {
  setCorsHeaders();
  httpServer.send(200);
}

// ==================== Web 页面 (嵌入 HTML) ====================

// HTML 页面存储在 Flash 中, 通过 send_P 发送
const char INDEX_HTML[] PROGMEM =
"<!DOCTYPE html>\n"
"<html lang='zh-CN'>\n"
"<head>\n"
"<meta charset='UTF-8'>\n"
"<meta name='viewport' content='width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no'>\n"
"<title>JK BMS 蓝牙中继后台</title>\n"
"<style>\n"
"*{margin:0;padding:0;box-sizing:border-box;-webkit-tap-highlight-color:transparent}\n"
"html,body{height:100%;overflow:hidden}\n"
"body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,'Helvetica Neue',sans-serif;background:#0f1320;color:#e0e6ed;font-size:14px;display:flex;flex-direction:column}\n"
".app{display:flex;flex-direction:column;height:100vh;max-width:500px;margin:0 auto;background:#0f1320;position:relative}\n"
".status-bar{display:none}\n"
".status-bar .time{font-weight:600;color:#fff}\n"
".status-bar .icons{display:flex;gap:6px;align-items:center}\n"
".status-bar .icon{width:16px;height:10px;background:#e0e6ed;border-radius:2px;position:relative}\n"
".status-bar .battery{width:24px;height:11px;border:1px solid #8892b0;border-radius:2px;position:relative}\n"
".status-bar .battery::after{content:'';position:absolute;right:-3px;top:3px;width:2px;height:5px;background:#8892b0}\n"
".status-bar .battery-fill{width:80%;height:100%;background:#2ed573;border-radius:1px}\n"
".header{padding:12px 16px;background:transparent;border-bottom:none}\n"
".header-top{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}\n"
".app-title{font-size:20px;font-weight:700;color:#fff}\n"
".status-pills{display:flex;gap:8px;align-items:center}\n"
".pill{display:inline-flex;align-items:center;gap:4px;padding:4px 10px;border-radius:12px;font-size:11px;font-weight:600}\n"
".pill-ble{background:rgba(100,255,218,.15);color:#64ffda}\n"
".pill-wifi{background:rgba(46,213,115,.15);color:#2ed573}\n"
".pill-ota{background:rgba(255,165,2,.15);color:#ffa502}\n"
".pill.active{opacity:1}\n"
".pill.inactive{opacity:.4}\n"
".pill-dot{width:6px;height:6px;border-radius:50%;background:currentColor}\n"
".version{font-size:12px;color:#8892b0}\n"
".content{flex:1;overflow-y:auto;padding:12px;padding-bottom:16px}\n"
".section{background:#161b2e;border-radius:12px;padding:16px;margin-bottom:12px}\n"
".section-title{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}\n"
".section-title h2{font-size:16px;font-weight:600;color:#fff}\n"
".section-title .action{font-size:12px;color:#64ffda;cursor:pointer;background:none;border:none}\n"
"/* ===== 系统状态卡片 ===== */\n"
".sys-status-item{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid rgba(255,255,255,.05);font-size:13px}\n"
".sys-status-item:last-child{border-bottom:none}\n"
".sys-status-item .label{color:#8892b0;white-space:nowrap}\n"
".sys-status-item .value{color:#e0e6ed;font-size:12px;font-family:'SF Mono',Monaco,Consolas,monospace;text-align:right}\n"
".sys-status-item .value-group{display:flex;align-items:center;gap:12px;text-align:right}\n"
".sys-status-item .value.ok{color:#2ed573}\n"
".sys-status-item .value.err{color:#ff6b6b}\n"
".sys-status-item .value.warn{color:#ffc048}\n"
".data-row{display:grid;grid-template-columns:repeat(5,1fr);gap:6px;margin-top:12px}\n"
".data-cell{text-align:center}\n"
".data-cell .label{font-size:10px;color:#8892b0;margin-bottom:4px}\n"
".data-cell .value{font-size:16px;font-weight:700;color:#fff}\n"
".data-cell .value .unit{font-size:11px;color:#8892b0;margin-left:2px}\n"
".data-cell.voltage .value{color:#64ffda}\n"
".config-row{display:flex;justify-content:space-between;align-items:center;padding:12px 0;border-bottom:1px solid rgba(255,255,255,.05)}\n"
".config-row:last-child{border-bottom:none}\n"
".config-label{font-size:14px;color:#e0e6ed}\n"
".inline-row{display:flex;align-items:center;gap:8px;margin-top:8px}\n"
".inline-row .config-label{flex:0 0 70px;white-space:nowrap;font-size:13px;color:#8892b0}\n"
".inline-row .config-input{flex:1;margin-top:0}\n"
".inline-row .btn-group{display:flex;gap:6px;flex:0 0 120px}\n"
".inline-row .btn-group .btn{flex:1;min-width:0;padding:7px 8px;font-size:13px}\n"
".config-value{font-size:14px;color:#64ffda;font-family:'SF Mono',Monaco,Consolas,monospace}\n"
".config-value.mac{font-size:12px}\n"
".config-input{width:100%;padding:8px 12px;background:#0a0e17;border:1px solid rgba(255,255,255,.1);border-radius:8px;color:#fff;font-size:14px;margin-top:8px}\n"
".config-input:focus{outline:none;border-color:#64ffda}\n"
".config-input-row{display:flex;flex-direction:row;flex-wrap:wrap}\n"
".config-input-row>div{flex:1;min-width:120px}\n"
".btn-row{display:flex;gap:8px;margin-top:12px}\n"
".btn{flex:1;padding:8px 14px;border:none;border-radius:8px;font-size:13px;font-weight:600;cursor:pointer;transition:all .2s;text-align:center;display:flex;align-items:center;justify-content:center}\n"
".btn:active{transform:scale(.97)}\n"
".btn-connect{background:linear-gradient(135deg,#2ed573,#7bed9f);color:#0a0e17}\n"
".btn-disconnect{background:linear-gradient(135deg,#ff4757,#ff6b81);color:#fff}\n"
".btn-save{background:linear-gradient(135deg,#3742fa,#5352ed);color:#fff}\n"
".btn-wifi-off{background:linear-gradient(135deg,#ff6348,#ff4757);color:#fff}\n"
".btn-wifi-on{background:linear-gradient(135deg,#2ed573,#7bed9f);color:#0a0e17}\n"
".btn-wifi{background:linear-gradient(135deg,#ffa502,#ff7f50);color:#fff}\n"
".wifi-status{display:flex;justify-content:space-between;align-items:center;margin-bottom:10px}\n"
".wifi-status-label{font-size:14px;color:#fff;font-weight:500}\n"
".wifi-status-value{font-size:13px;font-weight:600}\n"
".wifi-on{color:#2ed573}\n"
".wifi-off{color:#ff4757}\n"
".btn-scan{background:linear-gradient(135deg,#667eea,#764ba2);color:#fff;width:100%;margin-bottom:12px}\n"
".btn-full{width:100%}\n"
".scan-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}\n"
".scan-header h3{font-size:14px;font-weight:600;color:#fff}\n"
".scan-count{font-size:12px;color:#8892b0}\n"
".scan-list{max-height:200px;overflow-y:auto}\n"
".scan-item{display:flex;justify-content:space-between;align-items:center;padding:10px 12px;background:#0a0e17;border-radius:8px;margin-bottom:6px;cursor:pointer;transition:background .2s}\n"
".scan-item:hover{background:rgba(100,255,218,.1)}\n"
".scan-item.selected{background:rgba(100,255,218,.15);border:1px solid #64ffda}\n"
".scan-item-info{flex:1}\n"
".scan-item-name{font-size:14px;color:#fff;font-weight:500}\n"
".scan-item-mac{font-size:11px;color:#8892b0;margin-top:2px;font-family:'SF Mono',Monaco,Consolas,monospace}\n"
".scan-item-meta{display:flex;gap:8px;font-size:10px;color:#8892b0;margin-top:2px}\n"
".scan-item-meta span{background:rgba(255,255,255,.05);padding:1px 6px;border-radius:3px}\n"
".scan-item-rssi{font-size:13px;font-weight:600;color:#64ffda}\n"
".scan-item-rssi.weak{color:#ff4757}\n"
".scan-item-rssi.medium{color:#ffa502}\n"
".log-section{background:#161b2e;border-radius:12px;padding:16px;margin-bottom:0}\n"
".log-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}\n"
".log-header h2{font-size:16px;font-weight:600;color:#fff}\n"
".log-clear{font-size:12px;color:#8892b0;background:none;border:none;cursor:pointer;padding:4px 8px}\n"
".log-clear:hover{color:#ff4757}\n"
".log-list{max-height:150px;overflow-y:auto;background:#0a0e17;border-radius:8px;padding:8px}\n"
".log-item{font-family:'SF Mono',Monaco,Consolas,monospace;font-size:11px;color:#8892b0;padding:4px 0;line-height:1.4;word-break:break-all}\n"
".log-item .time{color:#64ffda}\n"
".log-item .msg{color:#e0e6ed}\n"
".log-empty{text-align:center;color:#4a5568;font-size:12px;padding:20px}\n"
".bottom-bar{display:none}\n"
".bar-btn{width:44px;height:44px;display:flex;align-items:center;justify-content:center;background:rgba(255,255,255,.1);border-radius:12px;cursor:pointer;transition:background .2s}\n"
".bar-btn:active{background:rgba(255,255,255,.2)}\n"
".bar-btn svg{width:22px;height:22px;stroke:#e0e6ed;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}\n"
".bar-ip{font-size:13px;color:#64ffda;font-weight:600;padding:0 12px;font-family:'SF Mono',Monaco,Consolas,monospace}\n"
".toast{position:fixed;top:50%;left:50%;transform:translate(-50%,-50%) scale(.9);background:rgba(0,0,0,.9);color:#fff;padding:16px 24px;border-radius:12px;font-size:14px;z-index:1000;opacity:0;transition:all .2s}\n"
".toast.show{opacity:1;transform:translate(-50%,-50%) scale(1)}\n"
".toast.error{background:rgba(255,71,87,.9)}\n"
".modal{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,.6);display:none;align-items:center;justify-content:center;z-index:200;padding:20px}\n"
".modal.show{display:flex}\n"
".modal-content{background:#161b2e;border-radius:16px;padding:24px;width:100%;max-width:300px}\n"
".modal-title{font-size:16px;font-weight:600;color:#fff;margin-bottom:12px}\n"
".modal-msg{font-size:14px;color:#e0e6ed;margin-bottom:20px;white-space:pre-line}\n"
".modal-btns{display:flex;gap:10px}\n"
".modal-btn{flex:1;padding:10px;border-radius:8px;border:none;font-size:14px;font-weight:500;cursor:pointer}\n"
".modal-btn-cancel{background:rgba(255,255,255,.1);color:#fff}\n"
".modal-btn-confirm{background:#3742fa;color:#fff}\n"
".loading{display:inline-block;width:14px;height:14px;border:2px solid rgba(255,255,255,.3);border-top-color:#fff;border-radius:50%;animation:spin 1s linear infinite}\n"
"@keyframes spin{to{transform:rotate(360deg)}}\n"
"::-webkit-scrollbar{width:0px;height:4px}\n"
"::-webkit-scrollbar-track{background:transparent}\n"
"::-webkit-scrollbar-thumb{background:rgba(255,255,255,.1);border-radius:2px}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class='app'>\n"
"<div class='header'>\n"
"<div class='header-top'>\n"
"<span class='app-title' id='appTitle'>JK-BMS 4G 远程监控网关</span>\n"
"</div>\n"
"</div>\n"
"<div class='content' id='content'>\n"
"<div class='section'>\n"
"<div class='section-title'><h2>系统状态</h2></div>\n"
"<div class='sys-status-item'><span class='label'>保护板(BMS)</span><span class='value-group'><span class='value' id='sBmsMac'>--</span><span class='value err' id='sBms'>未连接</span></span></div>\n"
"<div class='sys-status-item'><span class='label'>中继状态</span><span class='value' id='sBleSrv'>--</span></div>\n"
"<div class='sys-status-item'><span class='label'>剩余可用内存</span><span class='value' id='sHeapFree'>--</span></div>\n"
"<div class='sys-status-item'><span class='label'>上次重启</span><span class='value' id='sResetInfo'>--</span></div>\n"
"</div>\n"
"<div class='section' id='bmsDataCard'>\n"
"<div class='section-title'><h2>BMS 实时数据</h2></div>\n"
"<div class='data-row'>\n"
"<div class='data-cell'><div class='label'>总压</div><div class='value' id='totalVoltage'>--<span class='unit'>V</span></div></div>\n"
"<div class='data-cell'><div class='label'>电流</div><div class='value' id='current'>--<span class='unit'>A</span></div></div>\n"
"<div class='data-cell'><div class='label'>电量</div><div class='value' id='soc'>--<span class='unit'>%</span></div></div>\n"
"<div class='data-cell voltage'><div class='label'>压差</div><div class='value' id='cellDelta'>--<span class='unit'>V</span></div></div>\n"
"<div class='data-cell'><div class='label'>温度</div><div class='value' id='maxTemp'>--<span class='unit'>°C</span></div></div>\n"
"</div>\n"
"<div style='margin-top:12px;font-size:12px;color:#8892b0;display:flex;align-items:center;cursor:pointer;user-select:none;padding:4px 0' onclick='toggleCells()'>\n"
"<span style='color:#64ffda;font-weight:600'>单体详情</span>\n"
"<span id='cellToggleIcon' style='margin-left:auto;color:#64ffda;font-size:10px;transition:transform 0.2s;transform:rotate(-90deg)'>&#9654;</span>\n"
"</div>\n"
"<div id='cellVoltageWrap' style='margin-top:8px;max-height:0px;overflow:hidden;transition:max-height 0.3s ease'>\n"
"<div style='font-size:12px;color:#8892b0;margin-bottom:4px;text-align:center'>串数: <span id='cellCount' style='color:#64ffda;font-weight:600'>--</span> 串</div>\n"
"<div id='cellVoltageList' style='display:grid;grid-template-columns:repeat(4,1fr);gap:4px'></div>\n"
"</div>\n"
"</div>\n"
"<div class='section'>\n"
"<div class='section-title'><h2>蓝牙扫描</h2></div>\n"
"<button class='btn btn-save' onclick='scanBms()' style='width:100%'>扫描保护板</button>\n"
"<div id='scanResult' style='font-size:12px;margin-top:8px'></div>\n"
"</div>\n"
"<div class='section'>\n"
"<div class='section-title'><h2>BLE 中继</h2></div>\n"
"<div class='config-input-row' style='gap:12px'>\n"
"<div style='flex:1'>\n"
"<div class='config-label'>BMS-MAC</div>\n"
"<input type='text' class='config-input' id='bmsMacInput' maxlength='19' placeholder='aa:bb:cc:dd:ee:ff'>\n"
"<div style='display:flex;gap:6px;margin-top:10px'>\n"
"<button class='btn btn-connect' onclick='connectBMS()' style='flex:1'>连接</button>\n"
"<button class='btn btn-disconnect' onclick='disconnectBMS()' style='flex:1'>断开</button>\n"
"</div>\n"
"</div>\n"
"<div style='flex:1'>\n"
"<div class='config-label'>中继名称</div>\n"
"<input type='text' class='config-input' id='relayName' maxlength='31' placeholder='JK-BMS-Relay'>\n"
"<div style='margin-top:10px'>\n"
"<button class='btn btn-save' onclick='saveRelayName()' style='width:100%'>保存中继名称</button>\n"
"</div>\n"
"</div>\n"
"</div>\n"
"</div>\n"
"<div class='section'>\n"
"<div class='wifi-status'>\n"
"<span class='wifi-status-label'>WiFi AP (10分钟无操作自动关闭)</span>\n"
"<span class='wifi-status-value' id='wifiStatus'>--</span>\n"
"</div>\n"
"<div class='config-input-row' style='gap:12px'>\n"
"<div>\n"
"<div class='config-label'>AP 名称：</div>\n"
"<input type='text' class='config-input' id='apSsidInput' maxlength='32' placeholder='JK-BMS-Config'>\n"
"</div>\n"
"<div>\n"
"<div class='config-label'>AP 密码：</div>\n"
"<input type='text' class='config-input' id='apPasswordInput' maxlength='63' placeholder='至少8位'>\n"
"</div>\n"
"</div>\n"
"<div class='btn-row'>\n"
"<button class='btn btn-save' onclick='saveApConfig()'>保存AP配置</button>\n"
"<button class='btn btn-wifi-off' id='wifiOffBtn' onclick='turnOffWifi()'>关闭WiFi</button>\n"
"<button class='btn btn-wifi-on' id='wifiOnBtn' style='display:none' onclick='turnOnWifi()'>开启WiFi</button>\n"
"</div>\n"
"</div>\n"
"<div class='log-section'>\n"
"<div class='log-header'>\n"
"<h2>最近日志</h2>\n"
"<div style='display:flex;align-items:center;gap:12px'>\n"
"<span id='barIp' style='font-size:12px;color:#64ffda;font-family:SF Mono,Monaco,Consolas,monospace'>--</span>\n"
"<button class='log-clear' onclick='clearLogs()'>清除日志</button>\n"
"</div>\n"
"</div>\n"
"<div class='log-list' id='logList'>\n"
"<div class='log-empty'>暂无日志</div>\n"
"</div>\n"
"</div>\n"
"</div>\n"
"</div>\n"
"<div class='toast' id='toast'></div>\n"
"<div class='modal' id='modal'>\n"
"<div class='modal-content'>\n"
"<div class='modal-title' id='modalTitle'>确认</div>\n"
"<div class='modal-msg' id='modalMsg'>确定要执行此操作吗？</div>\n"
"<div class='modal-btns'>\n"
"<button class='modal-btn modal-btn-cancel' onclick='closeModal()'>取消</button>\n"
"<button class='modal-btn modal-btn-confirm' id='modalConfirm'>确定</button>\n"
"</div>\n"
"</div>\n"
"</div>\n"
"<div style='position:fixed;right:10px;bottom:6px;font-size:10px;color:rgba(255,255,255,.35);z-index:99;pointer-events:none'>v2.15</div>\n"
"<script>\n"
"const state={ws:null,connected:false,logs:[]};\n"
"function $(id){return document.getElementById(id)}\n"
"function showToast(msg,isErr){const t=$('toast');t.textContent=msg;t.className='toast'+(isErr?' error':'');requestAnimationFrame(()=>t.classList.add('show'));setTimeout(()=>t.classList.remove('show'),2000)}\n"
"function addLog(msg){const t=new Date();const ts=String(t.getHours()).padStart(2,'0')+':'+String(t.getMinutes()).padStart(2,'0')+':'+String(t.getSeconds()).padStart(2,'0');state.logs.unshift({ts,msg});if(state.logs.length>50)state.logs.pop();renderLogs()}\n"
"function renderLogs(){const el=$('logList');if(state.logs.length===0){el.innerHTML='<div class=\\'log-empty\\'>暂无日志</div>';return}el.innerHTML=state.logs.map(l=>'<div class=\\'log-item\\'><span class=\\'time\\'>['+l.ts+']</span> <span class=\\'msg\\'>'+l.msg+'</span></div>').join('')}\n"
"function clearLogs(){state.logs=[];renderLogs()}\n"
"function openModal(title,msg,onConfirm){$('modalTitle').textContent=title;$('modalMsg').textContent=msg;$('modalConfirm').onclick=()=>{closeModal();onConfirm&&onConfirm()};$('modal').classList.add('show')}\n"
"function closeModal(){$('modal').classList.remove('show')}\n"
"var cellsExpanded=false;function toggleCells(){cellsExpanded=!cellsExpanded;var w=$('cellVoltageWrap');var i=$('cellToggleIcon');if(cellsExpanded){w.style.maxHeight='2000px';i.style.transform='';i.innerHTML='&#9660;'}else{w.style.maxHeight='0px';i.style.transform='rotate(-90deg)';i.innerHTML='&#9654;'}}\n"
"function setStatusEl(id,text,cls){const el=$(id);if(!el)return;el.textContent=text;el.className='value '+(cls||'')}\n"
"function updateSysStatus(d){if(d.bmsConnected){setStatusEl('sBms','已连接('+(d.bmsRssi||0)+'dB)','ok')}else{setStatusEl('sBms','未连接','err')}setStatusEl('sBmsMac',d.bmsMac||'--','');const conn=(d.bleServerConnCount||0)>0;setStatusEl('sBleSrv',conn?('广播中('+d.bleServerConnCount+'个客户端已连接)'):'待机中',conn?'ok':'warn');const hf=d.heapFree||0;setStatusEl('sHeapFree',(hf/1024).toFixed(0)+' KB',(hf<30000)?'err':(hf<60000)?'warn':'ok');setStatusEl('sResetInfo',d.lastResetInfo||'上次重启: --','')}\n"
"function updateBmsData(d){if(d.valid){state.connected=true;$('totalVoltage').innerHTML=d.totalVoltage.toFixed(2)+'<span class=\\'unit\\'>V</span>';$('current').innerHTML=(d.current>=0?'+':'')+d.current.toFixed(2)+'<span class=\\'unit\\'>A</span>';$('soc').innerHTML=d.soc+'<span class=\\'unit\\'>%</span>';$('cellDelta').innerHTML=d.deltaCellVoltage.toFixed(3)+'<span class=\\'unit\\'>V</span>';const maxT=Math.max(d.mosfetTemp||0,d.temp1||0,d.temp2||0);$('maxTemp').innerHTML=maxT.toFixed(1)+'<span class=\\'unit\\'>°C</span>';$('cellCount').textContent=d.cellCount||'--';const cells=d.cellVoltages||[];let html='';for(let i=0;i<cells.length;i++){html+='<div style=\\'background:#0a0e17;border-radius:4px;padding:4px;text-align:center\\'><div style=\\'font-size:9px;color:#8892b0\\'>#'+(i+1)+'</div><div style=\\'font-size:12px;font-weight:600;color:'+(cells[i]<3.0?'#ff4757':'#fff')+'\\'>'+cells[i].toFixed(3)+'</div></div>'}$('cellVoltageList').innerHTML=html}else{state.connected=false;$('totalVoltage').innerHTML='--<span class=\\'unit\\'>V</span>';$('current').innerHTML='--<span class=\\'unit\\'>A</span>';$('soc').innerHTML='--<span class=\\'unit\\'>%</span>';$('cellDelta').innerHTML='--<span class=\\'unit\\'>V</span>';$('maxTemp').innerHTML='--<span class=\\'unit\\'>°C</span>';$('cellCount').textContent='--';$('cellVoltageList').innerHTML=''}}\n"
"function connectWebSocket(){const p=location.protocol==='https:'?'wss:':'ws:';state.ws=new WebSocket(p+'//'+location.hostname+':81');state.ws.onopen=()=>{};state.ws.onmessage=e=>{const d=JSON.parse(e.data);if(typeof d.bmsConnected!=='undefined'){updateSysStatus(d)}else if(typeof d.diag!=='undefined'){addLog(d.diag)}else{updateBmsData(d)}};state.ws.onclose=()=>{setTimeout(connectWebSocket,5000)}}\n"
"async function fetchData(){try{const d=await(await fetch('/api/data')).json();updateBmsData(d)}catch(e){}}\n"
"async function fetchStatus(){try{const d=await(await fetch('/api/status')).json();updateSysStatus(d)}catch(e){}}\n"
"async function fetchConfig(){try{const d=await(await fetch('/api/config')).json();$('relayName').value=d.relayName||'';$('bmsMacInput').value=d.bmsMac||'';$('apSsidInput').value=d.apSsid||'';$('apPasswordInput').value=d.apPassword||'';updateWifiUI(d.wifiEnabled);addLog('配置已加载')}catch(e){}}\n"
"async function scanBms(){const box=$('scanResult');box.innerHTML='扫描中(约3秒)...';try{const r=await fetch('/api/scan',{method:'POST'});if(!r.ok)return;for(let i=0;i<20;i++){await new Promise(res=>setTimeout(res,300));const s=await(await fetch('/api/scan/status')).json();if(!s.scanning)break}const res=await(await fetch('/api/scan/result')).json();if(!res.devices||!res.devices.length){box.innerHTML='未发现蓝牙设备';return}box.innerHTML='';res.devices.forEach(dev=>{const row=document.createElement('div');row.style.cssText='display:flex;justify-content:space-between;padding:7px 8px;border-bottom:1px solid #0f1626;cursor:pointer';row.onclick=()=>useBms(dev.mac,dev.name);const nm=document.createElement('span');nm.textContent=dev.name||'(无名称)';nm.style.color=dev.name?'#e6f1ff':'#8892b0';const mc=document.createElement('span');mc.style.cssText='color:#64ffda;font-family:monospace';mc.textContent=dev.mac;const rs=document.createElement('span');rs.textContent=dev.rssi+'dBm';row.appendChild(nm);row.appendChild(mc);row.appendChild(rs);box.appendChild(row)})}catch(e){box.innerHTML='扫描失败'}}\n"
"function useBms(mac,name){$('bmsMacInput').value=mac;openModal('连接 BMS','确定连接 '+mac+(name?' ('+name+')':'')+' 吗？',async()=>{try{addLog('通过扫描选择 BMS: '+mac);const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({bmsMac:mac})});const d=await r.json();if(d.success){showToast('MAC已保存, 正在连接...');setTimeout(()=>{fetch('/api/reconnect',{method:'POST'})},500)}else{showToast('保存失败',true)}}catch(e){showToast('连接失败',true)}})}\n"
"async function connectBMS(){const mac=$('bmsMacInput').value.trim();if(!mac){showToast('请先输入 MAC 地址',true);return}openModal('连接 BMS','确定要连接 '+mac+' 吗？\\n(将自动保存MAC)',async()=>{try{addLog('正在连接 BMS: '+mac);const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({bmsMac:mac})});const d=await r.json();if(d.success){showToast('MAC已保存, 正在连接...');setTimeout(()=>{fetch('/api/reconnect',{method:'POST'});addLog('BMS 连接请求已发送')},500)}else{showToast('保存失败',true)}}catch(e){showToast('连接失败',true)}})}\n"
"async function disconnectBMS(){openModal('断开 BMS','确定要断开当前 BMS 连接吗？',async()=>{try{await fetch('/api/reconnect',{method:'POST'});addLog('已断开 BMS 连接')}catch(e){}})}\n"
"async function saveRelayName(){const n=$('relayName').value.trim();if(!n){showToast('名称不能为空',true);return}openModal('保存中继名','保存后将自动重启设备使BLE广播名称生效,确定？',async()=>{try{const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({relayName:n})});const d=await r.json();if(d.success){showToast('中继名已保存, 正在重启...');addLog('中继名已保存: '+n+'，正在重启...');setTimeout(()=>{fetch('/api/reboot',{method:'POST'})},1500)}}catch(e){showToast('保存失败',true)}})}\n"
"function updateWifiUI(enabled){const s=$('wifiStatus');if(enabled){s.textContent='已开启';s.className='wifi-status-value wifi-on';$('wifiOffBtn').style.display='flex';$('wifiOnBtn').style.display='none'}else{s.textContent='已关闭';s.className='wifi-status-value wifi-off';$('wifiOffBtn').style.display='none';$('wifiOnBtn').style.display='flex'}}\n"
"async function saveApConfig(){const s=$('apSsidInput').value.trim();const p=$('apPasswordInput').value.trim();if(!s){showToast('AP名称不能为空',true);return}if(p.length>0&&p.length<8){showToast('密码至少8位',true);return}openModal('保存AP配置','保存后将自动重启设备生效,确定？',async()=>{try{const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({apSsid:s,apPassword:p})});const d=await r.json();if(d.success){showToast('AP配置已保存, 正在重启...');addLog('AP配置已保存: SSID='+s+'，正在重启...');setTimeout(()=>{fetch('/api/reboot',{method:'POST'})},1500)}}catch(e){showToast('保存失败',true)}})}\n"
"async function turnOffWifi(){openModal('关闭WiFi','关闭后仅BLE运行,节省资源,确定？',async()=>{try{await fetch('/api/wifi/off',{method:'POST'});addLog('WiFi已关闭 (BLE继续运行)');updateWifiUI(false);showToast('WiFi已关闭')}catch(e){showToast('操作失败',true)}})}\n"
"function uploadOta(){const f=$('otaFile').files[0];if(!f){showToast('请先选择 .bin 固件文件',true);return}if(!f.name.toLowerCase().endsWith('.bin')){showToast('请选择 .bin 文件',true);return}openModal('固件升级','将上传 '+f.name+' ('+(f.size/1024/1024).toFixed(2)+' MB)，上传完成后设备自动重启；失败自动回滚。确定？',()=>{const xhr=new XMLHttpRequest();xhr.open('POST','/api/ota',true);xhr.upload.onprogress=e=>{if(e.lengthComputable){$('otaProgress').style.display='block';$('otaProgressBar').style.width=Math.round(e.loaded/e.total*100)+'%'}};xhr.onload=()=>{try{const d=JSON.parse(xhr.responseText);$('otaStatus').textContent=d.success?('✅ '+d.message):('❌ '+d.message);if(d.success){showToast('升级成功, 设备重启中...')}else{showToast(d.message,true)}}catch(e){$('otaStatus').textContent='❌ 响应异常';showToast('升级失败',true)}};xhr.onerror=()=>{$('otaStatus').textContent='❌ 上传失败(连接中断)';showToast('上传失败',true)};const fd=new FormData();fd.append('fw',f);xhr.send(fd)};)}\n"
"async function turnOnWifi(){openModal('开启WiFi','正在开启WiFi,设备将短暂重启网络服务,确定？',async()=>{try{await fetch('/api/wifi/on',{method:'POST'});addLog('WiFi已开启');updateWifiUI(true);showToast('WiFi已开启')}catch(e){showToast('操作失败',true)}})}\n"
"async function init(){fetchConfig();fetchData();fetchStatus();connectWebSocket();const ip=window.location.hostname;$('barIp').textContent=ip;addLog('系统启动完成');addLog('访问地址: http://'+ip);setInterval(fetchData,3000);setInterval(fetchStatus,2000)}\n"
"init();\n"
"</script>\n"
"</body>\n"
"</html>";

// 主页 HTML
void handleRoot() {
  // v9.20: 禁用浏览器缓存 —— 固件升级后 HTML/JS 变化, 缓存旧页面会导致 JS 报错白屏"打不开"
  httpServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  httpServer.sendHeader("Pragma", "no-cache");
  httpServer.sendHeader("Expires", "0");
  httpServer.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

// ==================== BLE 连接管理 ====================

// --- BLE 服务端: 多客户端连接注册 / 订阅跟踪 (完整版 v8.1) ---

int registerConn(uint16_t handle) {
  for (int i = 0; i < MAX_BLE_CLIENTS; i++) {
    if (!g_connStates[i].active) {
      g_connStates[i].connHandle = handle;
      g_connStates[i].subscribed = false;
      g_connStates[i].active = true;
      g_bleClientCount++;  // v9.33
      return i;
    }
  }
  return -1;
}

// 注销连接, 同时减订阅计数
void unregisterConn(uint16_t handle) {
  for (int i = 0; i < MAX_BLE_CLIENTS; i++) {
    if (g_connStates[i].active && g_connStates[i].connHandle == handle) {
      if (g_connStates[i].subscribed && g_subscribedConnCount > 0) {
        g_subscribedConnCount--;
      }
      g_connStates[i].active = false;
      g_connStates[i].subscribed = false;
      if (g_bleClientCount > 0) g_bleClientCount--;  // v9.33
      break;
    }
  }
}

// 设置订阅状态
void setSubscribed(uint16_t handle, bool sub) {
  for (int i = 0; i < MAX_BLE_CLIENTS; i++) {
    if (g_connStates[i].active && g_connStates[i].connHandle == handle) {
      if (sub && !g_connStates[i].subscribed) {
        g_connStates[i].subscribed = true;
        g_subscribedConnCount++;
      } else if (!sub && g_connStates[i].subscribed) {
        g_connStates[i].subscribed = false;
        if (g_subscribedConnCount > 0) g_subscribedConnCount--;
      }
      break;
    }
  }
}

// ★FIX-1 (完整版): 帧级转发 —— 整帧 CRC 通过后, 按"各客户端协商 MTU"分块推送 (模拟真实 BMS 的大块通知)。
//   关键: NimBLE 的 notify() 不会自动分片, 超过连接协商 MTU 的数据会被静默截断!
//         所以必须按每个客户端 getPeerMTU()-3 分块; 既不能用 300 字节整包, 也不该统一 20 字节块。
void forwardFrameToClients(const uint8_t* frame, size_t len) {
  if (!pBleServerChar || !pBleServer) return;
  if (g_bleClientCount == 0 || g_subscribedConnCount == 0) return;  // v9.33: 自维护计数 (回调链安全)

  for (int i = 0; i < MAX_BLE_CLIENTS; i++) {
    if (!g_connStates[i].active || !g_connStates[i].subscribed) continue;
    uint16_t mtu = pBleServer->getPeerMTU(g_connStates[i].connHandle);
    size_t chunk = (mtu >= 23) ? (size_t)(mtu - 3) : 20;
    // ★FIX-13 (完整版): 分块大小模拟真实 BMS —— 抓包证实 BMS 固定用 150B 块 (每帧 2 块),
    //                  与真实 BMS 完全一致可确保 App 必然认; 上限受客户端 MTU 限制 (NimBLE 超 MTU 静默截断)
    if (chunk > 150) chunk = 150;
    if (chunk < 20) chunk = 20;
    for (size_t off = 0; off < len; off += chunk) {
      size_t cl = (len - off < chunk) ? (len - off) : chunk;
      pBleServerChar->setValue(frame + off, cl);
      pBleServerChar->notify(g_connStates[i].connHandle);
    }
  }
}

// ★FIX-12 (完整版): 把非帧数据块 (AT 心跳 / 命令帧回显等) 原样分块透传给所有订阅客户端。
//   与 forwardFrameToClients 同构, 但数据源是任意原始块 (不要求是完整帧)。
void forwardRawToClients(const uint8_t* data, size_t len) {
  if (!pBleServerChar || !pBleServer) return;
  if (g_bleClientCount == 0 || g_subscribedConnCount == 0) return;  // v9.33: 自维护计数 (回调链安全)
  for (int i = 0; i < MAX_BLE_CLIENTS; i++) {
    if (!g_connStates[i].active || !g_connStates[i].subscribed) continue;
    uint16_t mtu = pBleServer->getPeerMTU(g_connStates[i].connHandle);
    size_t chunk = (mtu >= 23) ? (size_t)(mtu - 3) : 20;
    if (chunk > 150) chunk = 150;
    if (chunk < 20) chunk = 20;
    for (size_t off = 0; off < len; off += chunk) {
      size_t cl = (len - off < chunk) ? (len - off) : chunk;
      pBleServerChar->setValue(data + off, cl);
      pBleServerChar->notify(g_connStates[i].connHandle);
    }
  }
}

// ★FIX-3 (完整版): 向单个客户端按 MTU 分块发送 (保留备用; 默认订阅后不补发, 见 onSubscribe)
void forwardFrameToClient(uint16_t connHandle, const uint8_t* frame, size_t len) {
  if (!pBleServerChar || !pBleServer) return;
  uint16_t mtu = pBleServer->getPeerMTU(connHandle);
  size_t chunk = (mtu >= 23) ? (size_t)(mtu - 3) : 20;
  if (chunk < 20) chunk = 20;
  for (size_t off = 0; off < len; off += chunk) {
    size_t cl = (len - off < chunk) ? (len - off) : chunk;
    pBleServerChar->setValue(frame + off, cl);
    pBleServerChar->notify(connHandle);
  }
}

// --- BLE 服务端回调 (处理极空 App 的连接/断开/配对) ---

// v9.7/FIX-24: 自维护服务端连接计数 —— 回调内不使用 NimBLE 内部查询 API
//   (getAddress().toString()/getConnectedCount()), 实测在连接回调内调用这些
//   会触发内部对象失效崩溃 (Load access fault)
int g_serverConnCount = 0;

class BleServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    uint16_t handle = connInfo.getConnHandle();
    int slot = registerConn(handle);
    g_serverConnCount++;
    if (slot < 0) {
      // FIX-16 (完整版): NimBLE 控制器允许 6 连接 (含 BMS 客户端), App 槽位只有 MAX_BLE_CLIENTS 个;
      //   超出的客户端被接受却收不到数据会 6 秒超时断开, 不如显式拒绝
      Serial.printf("[BLE-Server] 连接槽位已满 (%d), 拒绝 handle=%d\n", MAX_BLE_CLIENTS, handle);
      pServer->disconnect(handle);
      g_serverConnCount--;
      return;
    }
    Serial.printf("[BLE-Server] 客户端连接: handle=%d, 槽位=%d, 共%d\n", handle, slot, g_serverConnCount);

    // v9.6/FIX-23: 不在连接回调内操作广播; 广播启停统一交给 loop() 5 秒健康检查
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    uint16_t handle = connInfo.getConnHandle();
    unregisterConn(handle);
    if (g_serverConnCount > 0) g_serverConnCount--;
    Serial.printf("[BLE-Server] 客户端断开: handle=%d, reason=%d (剩余 %d, 已订阅=%d)\n",
      handle, reason, g_serverConnCount, g_subscribedConnCount);
  }
};

class BleCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string value = pCharacteristic->getValue();
    if (value.length() == 0) return;

    // ★FIX-11 (完整版): 打印命令帧内容前 6 字节 (确认 App 发的是什么命令: AA 55 90 EB cmd xx)
    if (value.length() >= 6) {
      Serial.printf("[BLE-Server] App 写入: %d bytes, 命令: %02X %02X %02X %02X %02X %02X\n",
        (int)value.length(), (uint8_t)value[0], (uint8_t)value[1], (uint8_t)value[2],
        (uint8_t)value[3], (uint8_t)value[4], (uint8_t)value[5]);
    } else {
      Serial.printf("[BLE-Server] App 写入: %d bytes\n", (int)value.length());
    }

    // 透明转发: 把 App 命令原样转发给真实 BMS (对齐参考代码)
    if (pWriteChar && bmsConnected) {
      pWriteChar->writeValue((uint8_t*)value.data(), value.length(), false);
      // ★FIX-2: App 请求后顺延 ESP32 自主轮询, 避免两条请求的响应在透传流中交错
      lastRequest = millis();
      Serial.printf("[BLE-Server] 转发 %d bytes 给 BMS\n", (int)value.length());
    } else {
      // v9.38: BMS 未连接时不丢弃, 缓存最近一条命令, BMS 连上后自动补发。
      //   场景 (用户实测): 显示屏/极空 App 先连上中继就发 0x97/0x96 指令, 此时 BMS 还在重连,
      //   原逻辑直接丢弃 → App/显示屏收不到响应一直等待; 缓存后 BMS 一连接成功就补发, App 立即有响应。
      if (value.length() > 0 && value.length() <= sizeof(g_pendingCmd)) {
        memcpy(g_pendingCmd, value.data(), value.length());
        g_pendingCmdLen = (int)value.length();
      }
      Serial.printf("[BLE-Server] BMS 未连接, 缓存命令 %d bytes (待补发)\n", (int)value.length());
    }
  }

  void onRead(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    Serial.println("[BLE-Server] App 读取特征");
  }

  void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    bool subscribed = (subValue & 0x0001) != 0;
    setSubscribed(connInfo.getConnHandle(), subscribed);
    Serial.printf("[BLE-Server] CCCD %s (handle=%d, subValue=0x%04X, 已订阅=%d)\n",
      subscribed ? "已订阅" : "已取消", connInfo.getConnHandle(), subValue, g_subscribedConnCount);

    // ★FIX-3 (完整版): 订阅后不再补发缓存旧帧 (真实 BMS 订阅后不会插旧帧; 补发会让 App 收到
    //           与自身请求不匹配的旧帧, 可能主动断开重连 / 查询设备信息失败)。
    //           数据由客户端自己请求 + ESP32 兜底轮询 (IDLE_POLL_INTERVAL) 供给。
  }

  void onStatus(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, int code) override {
  }
};

// ★FIX-11 (完整版): 辅助特征回调 (FFE2/FFE3/FF11/FF12 占位): 写忽略不转发 (这些是配置通道,
//             不参与 FFE1 数据流), 仅打印日志供诊断
class BleAuxCharCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    std::string value = pCharacteristic->getValue();
    Serial.printf("[BLE-Server] 辅助特征写入 %s: %d bytes (忽略, 不转发)\n",
      pCharacteristic->getUUID().toString().c_str(), (int)value.length());
  }
  void onRead(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override {
    Serial.printf("[BLE-Server] 辅助特征读取 %s (返回 0x00)\n",
      pCharacteristic->getUUID().toString().c_str());
  }
  void onSubscribe(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo, uint16_t subValue) override {
    Serial.printf("[BLE-Server] 辅助特征订阅 %s: 0x%04X (占位, 不推送数据)\n",
      pCharacteristic->getUUID().toString().c_str(), subValue);
  }
};
BleAuxCharCallbacks g_auxCB;

// CCCD 描述符回调 (空实现, 由 onSubscribe 统一处理)
class BleDescCallbacks : public NimBLEDescriptorCallbacks {
  void onWrite(NimBLEDescriptor* pDescriptor, NimBLEConnInfo& connInfo) override {}
  void onRead(NimBLEDescriptor* pDescriptor, NimBLEConnInfo& connInfo) override {}
};

BleServerCallbacks g_serverCB;
BleCharCallbacks g_charCB;
BleDescCallbacks g_descCB;

// 初始化 BLE 服务端 (完整模拟 JK-BMS 广播)
void initBleServer() {
  Serial.println("[BLE-Server] 初始化 BLE 服务端...");

  NimBLEDevice::setDeviceName(g_config.relayName);

  pBleServer = NimBLEDevice::createServer();
  pBleServer->setCallbacks(&g_serverCB);

  // 主服务 0xFFE0 (与真实 BMS 一致: FFE1 数据通道 + FFE2/FFE3 配置)
  pBleService = pBleServer->createService(SERVICE_UUID);

  // FFE1: 主数据通道 [N R W WNR] (所有 BMS 数据在此推送)
  pBleServerChar = pBleService->createCharacteristic(
    CHAR_UUID,
    NIMBLE_PROPERTY::READ |
    NIMBLE_PROPERTY::WRITE |
    NIMBLE_PROPERTY::WRITE_NR |
    NIMBLE_PROPERTY::NOTIFY
  );
  pBleServerChar->setCallbacks(&g_charCB);

  // CCCD 描述符 (通知订阅)
  NimBLEDescriptor* pCccDesc = pBleServerChar->createDescriptor(
    NimBLEUUID((uint16_t)0x2902),
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE,
    2
  );
  pCccDesc->setCallbacks(&g_descCB);
  uint8_t cccdDefault[2] = {0x00, 0x00};
  pCccDesc->setValue(cccdDefault, 2);

  // ★FIX-11 (完整版): FFE2 [N W] + FFE3 [W] 占位特征 (真实 BMS 存在, App 会枚举/订阅/写入)
  NimBLECharacteristic* pFFE2 = pBleService->createCharacteristic(
    CHAR_FFE2_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY);
  pFFE2->setCallbacks(&g_auxCB);
  pFFE2->createDescriptor(NimBLEUUID((uint16_t)0x2902), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, 2);

  NimBLECharacteristic* pFFE3 = pBleService->createCharacteristic(
    CHAR_FFE3_UUID, NIMBLE_PROPERTY::WRITE);
  pFFE3->setCallbacks(&g_auxCB);

  pBleService->start();

  // ★FIX-11 (完整版): 0xFF10 服务 (真实 BMS 存在): FF11 [N WNR] + FF12 [N WNR] 占位
  pVendorService = pBleServer->createService(VENDOR_SERVICE_UUID);
  NimBLECharacteristic* pFF11 = pVendorService->createCharacteristic(
    CHAR_FF11_UUID, NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  pFF11->setCallbacks(&g_auxCB);
  pFF11->createDescriptor(NimBLEUUID((uint16_t)0x2902), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, 2);

  NimBLECharacteristic* pFF12 = pVendorService->createCharacteristic(
    CHAR_FF12_UUID, NIMBLE_PROPERTY::WRITE_NR | NIMBLE_PROPERTY::NOTIFY);
  pFF12->setCallbacks(&g_auxCB);
  pFF12->createDescriptor(NimBLEUUID((uint16_t)0x2902), NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE, 2);

  pVendorService->start();

  Serial.println("[BLE-Server] 服务: FFE0(FFE1/FFE2/FFE3) + FF10(FF11/FF12)");

  // 配置广播数据
  // ★FIX-17/v9: 广播名使用中继名称 g_config.relayName (Web 后台可设, 未设置默认 "JK-RELAY";
  //             真实 BMS 广播的就是长名, 其他极空 App 搜到的都是中继名;
  //             只有 iOS 5.x 只读广播包名称字段, 短名 "JK" 会让它显示 JK 而非中继名)。
  //   BLE 广播包仅 31 字节: flags(2) + 完整名(2+len) + 服务列表(6) ≤ 31 → 名称截断到 20 字符。
  //   厂商数据 (模拟 JK-BMS) 原在广播里放不下 (真机同样放不下, 因为真机广播有长名), 移到扫描响应。
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();

  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);

  // 完整设备名 (截断 ≤20 字符保证广播包不超 31 字节)
  std::string relayName(g_config.relayName);
  if (relayName.length() > 20) {
    // ★FIX-18/v9.1: UTF-8 安全截断 —— 名称可能含中文等多字节字符, 直接 resize(20)
    //               会切断多字节序列产生非法 UTF-8 (App 显示乱码); 截断后回退删除不完整的尾字节
    relayName.resize(20);
    while (!relayName.empty() && ((uint8_t)relayName.back() & 0xC0) == 0x80) {
      relayName.pop_back();  // 删除被切断的 UTF-8 续字节
    }
    if (!relayName.empty() && ((uint8_t)relayName.back() & 0xC0) == 0xC0) {
      relayName.pop_back();  // 删除孤立的 UTF-8 首字节
    }
  }
  advData.setName(relayName);

  // 完整服务列表: FFE0 + FF10 (真实 BMS 结构, 原 E07F 已移除)
  std::vector<NimBLEUUID> uuids;
  uuids.push_back(SERVICE_UUID);
  uuids.push_back(VENDOR_SERVICE_UUID);
  advData.setCompleteServices16(uuids);

  pAdv->setAdvertisementData(advData);

  // 扫描响应: 厂商数据 (模拟 JK-BMS) + 服务数据 (广播包放不下, 移到此处)
  NimBLEAdvertisementData scanRsp;

  uint8_t manufData[12];
  manufData[0] = 0x65; manufData[1] = 0x0B;
  manufData[2] = 0x88; manufData[3] = 0xA0;
  manufData[4] = 0xC8; manufData[5] = 0x47;
  manufData[6] = 0x80; manufData[7] = 0x37;
  manufData[8] = 0x79; manufData[9] = 0x38;
  manufData[10] = 0x00; manufData[11] = 0x00;
  scanRsp.setManufacturerData(std::string((char*)manufData, 12));

  uint8_t svcData[4];
  svcData[0] = 0xE0; svcData[1] = 0xFF;
  svcData[2] = 0x01; svcData[3] = 0x00;
  scanRsp.setServiceData(SERVICE_UUID, std::string((char*)svcData + 2, 2));

  pAdv->setScanResponseData(scanRsp);

  // v9.41: 初始化不启动广播 —— BMS 未连接时对客户端隐身 (防抢连挤占 BMS 连接/MTU 协商);
  //   广播由 loop 广播健康检查在 bmsConnected=true 后自动拉起。
  // pAdv->start();

  Serial.printf("[BLE-Server] 广播已配置, 名称: %s (BMS 连接后自动启动)\n", g_config.relayName);
  Serial.println("[BLE-Server] 服务: FFE0(FFE1/FFE2/FFE3) + FF10(FF11/FF12)");
  Serial.println("[BLE-Server] 等待极空 App 连接...");
}

// --- BLE 客户端回调 (连接真实 BMS) ---

class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient*) {
    Serial.println("[BMS] 已连接");
    g_bmsScanFailCount = 0;  // v9.33: 连接成功清零失败计数
  }

  void onDisconnect(NimBLEClient* pcli, int reason) {
    bmsConnected = false;
    pWriteChar = nullptr;
    pNotifyChar = nullptr;
    g_bmsData.rssi = 0;
    Serial.printf("[BMS] 断开连接, reason=%d\n", reason);
    // v9.5/FIX-22: 断连后销毁 client 对象 —— NimBLE-Arduino 复用已断开的 NimBLEClient
    //   直接 connect 会因内部状态失效崩溃 (C3 实测 Load access fault);
    //   销毁后下次连接重新 createClient (官方推荐的重连模式)。
    if (pcli != nullptr) {
      NimBLEDevice::deleteClient(pcli);
    }
    pClient = nullptr;  // 强制下次连接重建
    frameBuffer.clear();
    g_advDevice = nullptr;
    g_doConnect = false;
  }
};

ClientCallbacks g_clientCB;

// --- BMS 扫描回调 (非阻塞: 扫到目标后设置标志) ---
class BmsScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) {
    std::string devMac = dev->getAddress().toString();
    // v9.33: 扫描失败退避计数 —— 扫到目标即清零 (说明链路可用)
    if (devMac == std::string(g_config.bmsMac)) {
      g_bmsScanFailCount = 0;
    }
    // v9.17: 手动扫描模式 (web 后台扫描保护板) —— 收集所有设备供前端选择
    if (g_manualScanning) {
      {
        const char* nm = dev->getName().c_str();
        // v9.36: 按 MAC 去重合并 —— 主动扫描同一设备会回调多次 (adv 包 + scan response),
        //   已存在则合并 (名称优先非空, 覆盖 RSSI), 不存在才追加。
        int found = -1;
        for (int i = 0; i < g_scanCount; i++) {
          if (strcmp(devMac.c_str(), g_scanList[i].mac) == 0) { found = i; break; }
        }
        if (found >= 0) {
          if (!g_scanList[found].name[0] && nm && nm[0]) {
            strncpy(g_scanList[found].name, nm, sizeof(g_scanList[found].name) - 1);
            g_scanList[found].name[sizeof(g_scanList[found].name) - 1] = '\0';
          }
          g_scanList[found].rssi = dev->getRSSI();
        } else if (g_scanCount < MAX_SCAN_ENTRIES) {
          strncpy(g_scanList[g_scanCount].mac, devMac.c_str(), sizeof(g_scanList[g_scanCount].mac) - 1);
          g_scanList[g_scanCount].mac[sizeof(g_scanList[g_scanCount].mac) - 1] = '\0';
          strncpy(g_scanList[g_scanCount].name, (nm && nm[0]) ? nm : "", sizeof(g_scanList[g_scanCount].name) - 1);
          g_scanList[g_scanCount].name[sizeof(g_scanList[g_scanCount].name) - 1] = '\0';
          g_scanList[g_scanCount].rssi = dev->getRSSI();
          g_scanCount++;
        }
      }
      return;  // 手动模式不触发自动连接
    }
    // v9.16: 打印扫描到的设备 (限流 5s) —— 定位"BMS 扫不到"是设备不在还是扫描失效
    static unsigned long lastScanDiag = 0;
    if (millis() - lastScanDiag > 5000) {
      lastScanDiag = millis();
      const char* name = dev->getName().c_str();
      Serial.printf("[SCAN] 看到设备: %s (%s) rssi=%d\n",
        devMac.c_str(), (name && name[0]) ? name : "无名称", dev->getRSSI());
    }
    if (devMac == std::string(g_config.bmsMac) && !bmsConnected && !g_doConnect) {
      Serial.printf("[BMS] 扫描到目标设备: %s\n", devMac.c_str());
      g_advDevice = dev;
      g_doConnect = true;
      NimBLEDevice::getScan()->stop();
    }
  }
  // v9.33: 扫描结束回调 —— 签名必须 (const NimBLEScanResults&, int) 才会被 NimBLE 调用!
  void onScanEnd(const NimBLEScanResults& results, int reason) {
    if (g_manualScanning) {
      g_manualScanning = false;  // 手动扫描结束标志 (JS 轮询 status 用)
      g_scanStarting = false;
      return;
    }
    if (!bmsConnected && !g_doConnect) {
      g_bmsScanFailCount++;
      if (g_bmsScanFailCount > 4) g_bmsScanFailCount = 4;  // 封顶: 退避上限 10s
      unsigned long nextIvl = RECONNECT_INTERVAL * (1 << g_bmsScanFailCount);
      if (nextIvl > 10000) nextIvl = 10000;
      Serial.printf("[BMS] 扫描无结果 (连续失败 %d 次, 下次 %.0fs 后)\n",
        g_bmsScanFailCount, nextIvl / 1000.0);
    }
  }
};

BmsScanCallbacks g_bmsScanCB;

// v9.17: 手动扫描 API (web 后台"扫描保护板")
// POST /api/scan      : 排队启动扫描, 立即返回
// GET  /api/scan/status : 返回扫描状态
// GET  /api/scan/result : 返回设备列表
void handleScan() {
  setCorsHeaders();
  if (g_manualScanning || g_scanStarting) {
    httpServer.send(429, "application/json", "{\"success\":false,\"msg\":\"scanning\"}");
    return;
  }
  g_scanCount = 0;
  g_manualScanning = true;
  g_scanStarting = true;   // loop 检测到后执行真正的扫描 (HTTP handler 不直接调 NimBLE, 防并发崩溃)
  httpServer.send(200, "application/json", "{\"success\":true,\"msg\":\"scanning\"}");
}

void handleScanStatus() {
  setCorsHeaders();
  String json = String("{\"success\":true,\"scanning\":") +
                (g_manualScanning ? "true" : "false") +
                ",\"count\":" + String(g_scanCount) + "}";
  httpServer.send(200, "application/json", json);
}

void handleScanResult() {
  setCorsHeaders();
  if (g_manualScanning) {
    httpServer.send(200, "application/json", "{\"success\":true,\"scanning\":true,\"devices\":[]}");
    return;
  }
  String json = "{\"success\":true,\"scanning\":false,\"devices\":[";
  for (int i = 0; i < g_scanCount; i++) {
    if (i > 0) json += ",";
    json += "{\"mac\":\"" + String(g_scanList[i].mac) +
            "\",\"name\":\"" + String(g_scanList[i].name) +
            "\",\"rssi\":" + String(g_scanList[i].rssi) + "}";
  }
  json += "]}";
  httpServer.send(200, "application/json", json);
}

// v9.33: 手动扫描执行器 —— 只在 loop 上下文调用 NimBLE (HTTP handler 只设标志)
void loopScanWork() {
  if (!g_scanStarting) return;
  g_scanStarting = false;
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->stop();  // 停掉可能正在运行的自动扫描
  delay(20);
  pScan->setScanCallbacks(&g_bmsScanCB);
  pScan->setActiveScan(true);
  pScan->setInterval(200);
  pScan->setWindow(150);
  bool started = pScan->start(3000, false, true);  // 3 秒
  if (!started) {
    g_manualScanning = false;
    Serial.println("[SCAN-M] 手动扫描 start 失败!");
  } else {
    Serial.println("[SCAN-M] 手动扫描已启动 (3s)...");
  }
}

// 扫描并连接 BMS (非阻塞: 启动异步扫描, 不阻塞主循环)
void startBmsConnect() {
  if (bmsConnected || g_doConnect) return;
  if (g_manualScanning) return;  // 手动扫描进行中, 不启动自动扫描 (避免冲突)

  NimBLEScan* pScan = NimBLEDevice::getScan();
  // v9.37: 扫描进行中则不重复启动 —— 修复"扫描持续占射频拖慢 WiFi/web 后台"
  if (pScan->isScanning()) return;

  unsigned long now = millis();
  // v9.33: 失败退避 —— 3s→6s→12s 封顶 10s (BMS 恢复后应尽快发现)
  unsigned long interval = RECONNECT_INTERVAL;
  if (g_bmsScanFailCount > 0) {
    interval = RECONNECT_INTERVAL * (1 << g_bmsScanFailCount);
    if (interval > 10000) interval = 10000;
  }
  if (now - lastReconnectAttempt < interval) return;
  lastReconnectAttempt = now;

  Serial.printf("[BMS] 启动异步扫描, 目标: %s\n", g_config.bmsMac);
  pScan->setScanCallbacks(&g_bmsScanCB);
  pScan->setActiveScan(true);
  pScan->setInterval(500);   // 20% 占空比
  pScan->setWindow(100);
  pScan->start(5000, false, true);  // 5 秒异步扫描
}

// 连接到已扫描到的 BMS (仅扫描到设备后调用一次)
bool connectToBms() {
  if (!g_advDevice) return false;

  Serial.println("[BMS] 正在连接...");

  if (pClient == nullptr) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&g_clientCB, false);
    // v9.14/FIX-27: 连接参数放宽到 30~50ms + 6s 监督超时 (抗多连接挤占)
    pClient->setConnectionParams(24, 40, 0, 600);
    pClient->setConnectTimeout(3000);  // v9.33: 8000→3000 (缩短同步阻塞, web 不卡)
  }

  if (!pClient->connect(g_advDevice)) {
    Serial.println("[BMS] 连接失败");
    g_bmsScanFailCount++;
    if (g_bmsScanFailCount > 4) g_bmsScanFailCount = 4;
    // v9.5/FIX-22: 连接失败同样销毁 client, 下次重连重建
    NimBLEDevice::deleteClient(pClient);
    pClient = nullptr;
    return false;
  }

  Serial.printf("[BMS] 已连接, MTU=%u\n", pClient->getMTU());

  // v9.40/v9.43: MTU 检查 —— MTU<100 协商异常 → 有限重试 → 仍低则降级接受 MTU=23 继续
  if (pClient->getMTU() < 100) {
    if (g_mtuRetryCount < MTU_MAX_RETRY) {
      g_mtuRetryCount++;
      Serial.printf("[BMS] MTU 协商失败 (%u<100, 第%d/%d次), 断开重试\n",
        pClient->getMTU(), g_mtuRetryCount, MTU_MAX_RETRY);
      g_bmsScanFailCount++;
      if (g_bmsScanFailCount > 4) g_bmsScanFailCount = 4;
      pClient->disconnect();
      NimBLEDevice::deleteClient(pClient);
      pClient = nullptr;
      return false;
    }
    g_mtuRetryCount = 0;
    Serial.printf("[BMS] MTU=%u 重试%d次仍低, 降级接受继续 (520 重连时可再协商)\n",
      pClient->getMTU(), MTU_MAX_RETRY);
  } else {
    g_mtuRetryCount = 0;
  }

  NimBLERemoteService* pService = pClient->getService(SERVICE_UUID);
  if (!pService) {
    Serial.println("[BMS] 未找到服务 0xFFE0");
    g_bmsScanFailCount++;
    if (g_bmsScanFailCount > 4) g_bmsScanFailCount = 4;
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);
    pClient = nullptr;
    return false;
  }

  pWriteChar = nullptr;
  pNotifyChar = nullptr;

  auto chars = pService->getCharacteristics(true);
  for (auto& c : chars) {
    if (c->getUUID() != CHAR_UUID) continue;
    uint16_t handle = c->getHandle();
    // ★FIX-19/v9.3: 属性驱动 + handle 优先的特征选择, 兼容所有固件版本:
    //   V20S 双 FFE1 (0x03 写 / 0x05 通知) → 各自命中;
    //   单 FFE1 (写+通知一体, 老版本 V17/V18/V19) → 同一特征被选为写和通知;
    //   其他 handle 布局 (如 0x03 写 / 0x04 通知) → 按属性自动归类, 不再被 else 分支覆盖指错写特征
    if (c->canWrite() || c->canWriteNoResponse()) {
      if (pWriteChar == nullptr || handle == 0x03) {
        pWriteChar = c;
      }
    }
    if (c->canNotify() || c->canIndicate()) {
      if (pNotifyChar == nullptr || handle == 0x05) {
        pNotifyChar = c;
      }
    }
  }

  if (!pWriteChar || !pNotifyChar) {
    // 兜底: 遍历所有特征 (含非 FFE1 的服务), 写特征兼容 WRITE 与 WRITE_NR
    for (auto& c : chars) {
      if (!pWriteChar && (c->canWrite() || c->canWriteNoResponse())) {
        pWriteChar = c;
      }
      if (!pNotifyChar && (c->canNotify() || c->canIndicate())) {
        pNotifyChar = c;
      }
    }
  }

  if (!pWriteChar || !pNotifyChar) {
    Serial.println("[BMS] 未找到必要的特征");
    g_bmsScanFailCount++;
    if (g_bmsScanFailCount > 4) g_bmsScanFailCount = 4;
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);  // v9.5/FIX-22: 失败即销毁, 避免复用失效对象
    pClient = nullptr;
    return false;
  }

  Serial.printf("[BMS] 写=0x%04X 通知=0x%04X\n",
    pWriteChar->getHandle(), pNotifyChar->getHandle());

  // ★FIX-20/v9.3: 第一参按特征能力订阅 (canNotify → CCCD 0x0001 通知 / 否则 0x0002 指示);
  //   v9.35: 第三参恢复 true (有响应写, 真实 BMS 会确认 CCCD 写, 订阅必然建立)
  if (!pNotifyChar->subscribe(pNotifyChar->canNotify(), notifyCB, true)) {
    Serial.println("[BMS] 订阅通知失败");
    g_bmsScanFailCount++;
    if (g_bmsScanFailCount > 4) g_bmsScanFailCount = 4;
    pClient->disconnect();
    NimBLEDevice::deleteClient(pClient);  // v9.5/FIX-22: 失败即销毁
    pClient = nullptr;
    return false;
  }

  bmsConnected = true;
  frameBuffer.clear();

  // v9.41: BMS 连接成功 → 立即恢复广播 (客户端可连接取数)。
  //   安全: 此处 loop 上下文 (connectToBms 由 loop 调用), 非 NimBLE 事件回调,
  //   不受 FIX-23/v9.6 "回调内不操作广播" 限制; 广播健康检查也会兜底拉起。
  if (pBleServer) {
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
    if (!pAdv->isAdvertising()) {
      pAdv->start();
      Serial.println("[BLE-Server] BMS 已连接, 广播启动");
    }
  }

  // 获取初始 RSSI
  g_bmsData.rssi = pClient->getRssi();

  // 发送初始命令 (无延时, 立即发送)
  sendCommand(CMD_DEVICE_INFO);
  sendCommand(CMD_CELL_INFO);
  lastRequest = millis();
  lastDataTime = millis();

  // v9.38: 补发 BMS 未连接期间缓存的 App 命令 (显示屏/极空 App 先连上中继发的 0x97/0x96)
  if (g_pendingCmdLen > 0 && pWriteChar) {
    pWriteChar->writeValue(g_pendingCmd, g_pendingCmdLen, false);
    Serial.printf("[BLE-Server] 补发缓存命令 %d bytes\n", g_pendingCmdLen);
    g_pendingCmdLen = 0;
    lastRequest = millis();  // 补发也算一次请求, 顺延自主轮询避免响应交错
  }

  Serial.println("[BMS] 连接成功");
  return true;
}

void notifyCB(NimBLERemoteCharacteristic* pChar,
              uint8_t* pData, size_t length, bool isNotify) {
  // ★ 诊断: 记录 BMS 推送的数据 (每10秒推送到Web后台)
  static unsigned long lastDiagPrint = 0;
  if (millis() - lastDiagPrint > 10000) {
    lastDiagPrint = millis();
    char diagMsg[128];
    snprintf(diagMsg, sizeof(diagMsg), "BMS通知 len=%zu 客户端=%d 订阅=%d",
      length, g_bleClientCount, g_subscribedConnCount);  // v9.33: 自维护计数 (FIX-24: 回调内不查 NimBLE API)
    // ★FIX-8 (完整版): 防越界 —— 通知块 < 4 字节时不能打印前 4 字节
    if (length >= 4) {
      Serial.printf("[DIAG] %s, 前4字节: %02X %02X %02X %02X\n", diagMsg, pData[0], pData[1], pData[2], pData[3]);
    } else {
      Serial.printf("[DIAG] %s\n", diagMsg);
    }
    // 推送到 Web 后台
    if (g_wifiEnabled) {
      String diagStr = "{\"diag\":\"";
      diagStr += diagMsg;
      diagStr += "\"}";
      webSocketServer.broadcastTXT(diagStr);
    }
  }

  // ★FIX-1/FIX-12 (完整版): 双通道转发 —— BMS 推送流里有两类数据, 必须区别对待:
  //   ① 数据帧 (55 AA EB 90 开头, 0x01/0x02/0x03): 帧级重组 + CRC 后干净转发 → 解决乱跳/异常帧
  //   ② 非帧数据 (AT\r\n 心跳 41 54 0D 0A、命令帧回显 AA 55 90 EB C8 等):
  //      原样透传给订阅客户端 → 解决 App 6 秒断开 (reason=531)。
  //      抓包证实: 真实 BMS 每 ~150ms 推一个 AT 心跳, 极空 App 靠它判断设备存活;
  //      若帧级过滤掉, App 收不到心跳 → 认为连接无响应 → 超时主动断开。
  bool isFrameHeader = (length >= 4 &&
                        pData[0] == 0x55 && pData[1] == 0xAA &&
                        pData[2] == 0xEB && pData[3] == 0x90);

  if (isFrameHeader) {
    // 数据帧首块: 清空半帧残留, 开始新帧
    // v9.34: 若 buffer 里已有上一帧尾带过来的合法帧头残留, 追加合并而非 clear,
    //          否则清掉合法残留会偶发丢帧; 只有无残留或残留不是帧头 (脏数据) 才清空重新对齐
    bool bufHasHdr = (frameBuffer.size() >= 4 &&
                      frameBuffer[0] == 0x55 && frameBuffer[1] == 0xAA &&
                      frameBuffer[2] == 0xEB && frameBuffer[3] == 0x90);
    if (frameBuffer.empty() || !bufHasHdr) {
      frameBuffer.clear();
    }
    frameBuffer.insert(frameBuffer.end(), pData, pData + length);
  } else if (length >= 2 && pData[0] == 0x41 && pData[1] == 0x54) {
    // ★FIX-14 (完整版): AT 心跳 (41 54...) 由真实 BMS 异步插入, 可能落在帧块之间;
    //               不能吸收进 buffer 污染组帧 (会 CRC 失败丢整帧), 原样透传 (App 自己会跳过)
    // v9.13/FIX-26: AT 心跳是真机 BMS 存活最强信号 (~150ms 一次), 必须刷新 lastDataTime,
    //                否则 App 订阅时轮询降为 30s, 真机不主动推 0x02 帧时 30s 内无完整 JK 帧
    //                → DATA_TIMEOUT(30s) 误判 → 主动断开重连 (表现为"不定时断开又连上")
    lastDataTime = millis();
    forwardRawToClients(pData, length);
    return;
  } else if (length >= 4 && pData[0] == 0xAA && pData[1] == 0x55 &&
             pData[2] == 0x90 && pData[3] == 0xEB) {
    // ★FIX-14 (完整版): 命令帧回显 (AA 55 90 EB C8..., 字节序与数据帧头 55 AA EB 90 不同),
    //               同样透传不吸收, 避免污染组帧
    // v9.13/FIX-26: 命令回显同样证明 BMS 存活, 刷新 lastDataTime
    lastDataTime = millis();
    forwardRawToClients(pData, length);
    return;
  } else if (!frameBuffer.empty()) {
    // 数据帧续块: 吸收进 buffer 继续组帧
    // v9.33: 上限保护 —— 脏数据流时 buffer 无限增长会耗尽堆 (OOM); 正常稳态 ≤600B
    if (frameBuffer.size() + length <= 600) {
      frameBuffer.insert(frameBuffer.end(), pData, pData + length);
    } else {
      frameBuffer.clear();  // 超限: 当前组帧作废, 重新对齐
    }
  } else {
    // 空闲时的其他非帧数据 (残留续块等): 原样透传 (App 丢弃无帧头垃圾, 与真实 BMS 丢块一致)
    forwardRawToClients(pData, length);
    return;
  }

  // 未攒满一帧, 继续等待 (先按完整帧处理, 溢出/坏帧统一在 CRC 失败时对齐)
  if (frameBuffer.size() < JK_FRAME_LEN) return;

  const uint8_t* raw = frameBuffer.data();
  uint8_t computedCRC = jkCRC(raw, JK_FRAME_LEN - 1);
  uint8_t remoteCRC = raw[JK_FRAME_LEN - 1];

  if (computedCRC != remoteCRC) {
    // ★FIX-6 (完整版): 坏帧不转发。打印诊断 (命令字节 + 缓冲长度)。
    //   buffer 可能 >400 (大 MTU 合并推送后损坏), 统一从 0 扫描下一个帧头对齐;
    //   找不到则清空, 等下一个帧头块重新开始。
    static unsigned long lastBadDiag = 0;
    if (millis() - lastBadDiag > 5000) {
      lastBadDiag = millis();
      Serial.printf("[DIAG] 坏帧丢弃: cmd=0x%02X bufLen=%d 前4=%02X %02X %02X %02X\n",
        frameBuffer[4], (int)frameBuffer.size(),
        frameBuffer[0], frameBuffer[1], frameBuffer[2], frameBuffer[3]);
    }
    size_t hdr = findFrameHeader(frameBuffer, 0);  // from=0: 不假设 buffer[0..3] 是帧头
    if (hdr != (size_t)-1 && hdr > 0) {
      // 帧头在 buffer 中段: 丢弃帧头之前的错位垃圾
      frameBuffer.erase(frameBuffer.begin(), frameBuffer.begin() + hdr);
    } else if (hdr == 0) {
      // ★FIX-15 (完整版): 帧头就在 buffer 开头但 CRC 失败 (帧数据被污染):
      //   只丢一帧长度, 保留可能已到达的下一帧头部 (大 MTU 合并推送场景), 复用 FIX-8 帧头验证
      if (frameBuffer.size() > JK_FRAME_LEN) {
        frameBuffer.erase(frameBuffer.begin(), frameBuffer.begin() + JK_FRAME_LEN);
        if (frameBuffer.size() < 4 ||
            frameBuffer[0] != 0x55 || frameBuffer[1] != 0xAA ||
            frameBuffer[2] != 0xEB || frameBuffer[3] != 0x90) {
          frameBuffer.clear();
        }
      } else {
        frameBuffer.clear();
      }
    } else {
      // 找不到帧头: 全清, 等下一个帧头块重新开始
      frameBuffer.clear();
    }
    return;
  }

  const uint8_t* frame = raw;
  const size_t frameLen = JK_FRAME_LEN;

  // ★FIX-9 (完整版): 帧类型判断 (真实 BMS 抓包 + 脚本 CRC 验证):
  //   cmd=0x02 → BMS 每 ~0.5s 自动推送的电芯帧 (300B), 布局与 parseCellInfo 的 off=32 完全吻合,
  //             是 Web 后台/4G 数据源
  //   cmd=0x03 → 0x97 请求的设备信息响应 (300B, ASCII 文本布局)
  //   cmd=0x01 → 0x96 请求的电池信息响应 (300B, 但电压区为 4 字节模式, 布局不同) → 仅转发不解析
  //   其他     → 仅转发
  if (frame[4] == 0x02) {
    memcpy(g_cellInfoFrame, frame, frameLen);
    g_cellInfoLen = frameLen;
    g_hasCellInfo = true;
    parseCellInfo(frame, frameLen);
    // ★ 诊断: 打印电芯帧关键字段 (每10秒推送到Web后台)
    static unsigned long lastFrameDiag = 0;
    if (millis() - lastFrameDiag > 10000) {
      lastFrameDiag = millis();
      char frameMsg[128];
      snprintf(frameMsg, sizeof(frameMsg), "电芯帧: %.2fV %.2fA SOC=%d%% %d串",
        g_bmsData.totalVoltage, g_bmsData.current, g_bmsData.soc, g_bmsData.cellCount);
      Serial.printf("[DIAG] %s\n", frameMsg);
      if (g_wifiEnabled) {
        String frameStr = "{\"diag\":\"";
        frameStr += frameMsg;
        frameStr += "\"}";
        webSocketServer.broadcastTXT(frameStr);
      }
    }
  } else if (frame[4] == 0x03) {
    memcpy(g_deviceInfoFrame, frame, frameLen);
    g_deviceInfoLen = frameLen;
    g_hasDeviceInfo = true;
    parseDeviceInfo(frame, frameLen);
  }
  // 其他帧类型 (0x01 电池信息响应等) 不缓存不解析, 仅帧级转发

  // ★FIX-9 (完整版): 任何 CRC 通过的帧都表示 BMS 存活, 统一更新数据时间 (防误判超时重连)
  lastDataTime = millis();

  // ★ 4G 上报 (本固件保留): 保存完整原始帧 (供 b:r,<hex> 串口上报)
  memcpy(lastRawFrame, frame, JK_FRAME_LEN);
  lastRawFrameLen = (uint16_t)JK_FRAME_LEN;

  // ★FIX-1 (完整版): 完整帧校验通过后, 按各客户端协商 MTU 分块转发 (帧级转发, 模拟真实 BMS)
  //   诊断: 每 10 秒打印一次通过的帧 (命令 + 长度)
  static unsigned long lastGoodDiag = 0;
  if (millis() - lastGoodDiag > 10000) {
    lastGoodDiag = millis();
    Serial.printf("[DIAG] 帧通过: cmd=0x%02X len=%d 客户端=%d 订阅=%d\n",
      frame[4], (int)frameLen,
      pBleServer ? pBleServer->getConnectedCount() : -1, g_subscribedConnCount);
  }
  forwardFrameToClients(frame, frameLen);

  pushBMSData();

  // ★FIX-8 (完整版): 处理完完整帧后, 保留已提前收到的下一帧头部字节 (BMS 大块通知可能一次带过帧边界),
  //             否则下一帧要从帧头重新收, 会偶发丢帧
  if (frameBuffer.size() > JK_FRAME_LEN) {
    frameBuffer.erase(frameBuffer.begin(), frameBuffer.begin() + JK_FRAME_LEN);
    // ★ 残留必须是合法帧头开头, 否则是错位垃圾, 清空等下一个帧头块
    if (frameBuffer.size() < 4 ||
        frameBuffer[0] != 0x55 || frameBuffer[1] != 0xAA ||
        frameBuffer[2] != 0xEB || frameBuffer[3] != 0x90) {
      frameBuffer.clear();
    }
  } else {
    frameBuffer.clear();
  }
}

// ==================== 4G/GPS 串口模块 (合并自 esp32s3.ino + 双模式定时上报) ====================

// 蓝牙状态字符串 (供 getblestatus / blestatus: 上报)
// ★ 串口发送保护: 所有发往 DTU 的数据统一走这里,
//   保证两次发送间隔 >= 250ms, 避免 DTU 把紧邻的多条命令/透传数据
//   粘连成一行 (如 config,get,csq 与 blestatus 同时发出 → config,csq...,error,1)
//   非阻塞实现: 间隔不足时排入 g_pendingTx, 由 flushPendingTx() 在 loop 中稍后发送,
//   严禁在发送函数中使用 delay() (会导致中断看门狗超时崩溃)
void sendSerial1(const String& s) {
  unsigned long now = millis();
  if (now - g_serialTxTime < 250) {
    g_pendingTx = s;   // 排队, 等待 loop 中的发送时机
    return;
  }
  SERIAL1.println(s);
  g_serialTxTime = now;
}

// 发送排队数据 (每次 loop 调用, 间隔够就发, 非阻塞)
void flushPendingTx() {
  if (g_pendingTx.length() == 0) return;
  unsigned long now = millis();
  if (now - g_serialTxTime < 250) return;
  String s = g_pendingTx;
  g_pendingTx = "";
  SERIAL1.println(s);
  g_serialTxTime = millis();
}

String bleStateStr() {
  if (bmsConnected) return "connected";
  if (g_doConnect || g_advDevice != nullptr) return "connecting";
  return "disconnected";
}

// 将最近一帧 BMS 原始帧以 b:r,<hex> 格式发送给 DTU (服务端 parser.php 解析)
void sendRawFrameToSERIAL1() {
  if (!bmsConnected) {
    sendSerial1("bms:error,not connected");
    return;
  }
  if (lastRawFrameLen == 0) {
    sendSerial1("bms:error,no data");
    return;
  }

  // 组装整行后统一走 sendSerial1 (间隔保护 + 非阻塞排队)
  String payload = "b:r,";
  for (uint16_t i = 0; i < lastRawFrameLen; i++) {
    char h[3];
    snprintf(h, sizeof(h), "%02X", lastRawFrame[i]);
    payload += h;
  }
  sendSerial1(payload);
  Serial.println("[4G] b:r 原始帧已发送到 DTU");
}

// 解析 mode,realtime/track,<bms_ms>,<gps_ms> 指令 (由 PHP 桥接进程经 MQTT R 主题 + DTU 下发)
void handleModeCommand(const String& clean) {
  int p1 = clean.indexOf(',', 5);   // "mode," 之后第一个逗号
  String mode = "track";
  unsigned long bmsMs = 0, gpsMs = 0;

  if (p1 > 0) {
    mode = clean.substring(5, p1);
    mode.toLowerCase();
    mode.trim();
    int p2 = clean.indexOf(',', p1 + 1);
    if (p2 > p1) {
      bmsMs = strtoul(clean.substring(p1 + 1, p2).c_str(), NULL, 10);
      gpsMs = strtoul(clean.substring(p2 + 1).c_str(), NULL, 10);
    }
  }

  if (mode != "realtime") mode = "track";
  g_reportMode = mode;

  if (bmsMs > 0) {
    g_bmsIntervalMs = bmsMs;
    if (mode == "realtime") g_realtimeBmsMs = bmsMs; else g_trackBmsMs = bmsMs;
  } else {
    g_bmsIntervalMs = (mode == "realtime") ? g_realtimeBmsMs : g_trackBmsMs;
  }
  if (gpsMs > 0) {
    g_gpsIntervalMs = gpsMs;
    if (mode == "realtime") g_realtimeGpsMs = gpsMs; else g_trackGpsMs = gpsMs;
  } else {
    g_gpsIntervalMs = (mode == "realtime") ? g_realtimeGpsMs : g_trackGpsMs;
  }

  // 切换后立即按新间隔上报一次
  g_lastBmsReport = 0;
  g_lastGpsReport = 0;
  Serial.printf("[4G] 上报模式: %s, BMS=%lums, GPS=%lums\n",
    g_reportMode.c_str(), g_bmsIntervalMs, g_gpsIntervalMs);
}

// 串口指令分发 (合并自 esp32s3.ino handleSERIAL1Data)
void handleSerial1Command(String data) {
  String clean = "";
  for (int i = 0; i < data.length(); i++) {
    char c = data[i];
    if (c >= 32 && c < 127) clean += c;
  }
  clean.trim();

  // ★ 自愈: 收到任何串口行 = DTU 串口链路活着; EMQX 下行指令 = 上下行链路通
  lastDtuResponse = millis();
  if (clean.startsWith("mode,") || clean.equalsIgnoreCase("chaxun") ||
      clean.equalsIgnoreCase("getcsq") || clean.equalsIgnoreCase("getgps") ||
      clean.equalsIgnoreCase("getblestatus")) {
    lastDownlink = millis();
  }

  Serial.print("[SERIAL1 收到] '");
  Serial.print(clean);
  Serial.println("'");

  if (clean.equalsIgnoreCase("chaxun")) {
    // 手动请求 BMS 数据上报
    sendRawFrameToSERIAL1();
  }

  else if (clean.equalsIgnoreCase("reboot")) {
    // ★ 远程重启 (网页"设置"页按钮 → EMQX 下行 → 此指令): 冷重启 DTU 后重启 ESP32
    Serial.println("[4G] 收到远程重启指令, 3 秒后重启...");
    if (DTU_POWER_PIN >= 0) {
      digitalWrite(DTU_POWER_PIN, LOW);
      delay(3000);
      digitalWrite(DTU_POWER_PIN, HIGH);
      delay(2000);
    }
    ESP.restart();
  }

  else if (clean.equalsIgnoreCase("getcsq")) {
    // 查询 4G 信号: 转给 DTU, DTU 回 config,csq,ok,<val> 后再回 csq:<val>
    sendSerial1("config,get,csq");
  }

  else if (clean.equalsIgnoreCase("getgps")) {
    // 查询 GPS: 转给 DTU, DTU 回 config,gps,ok,... 后再回 gps:...
    sendSerial1("config,get,gpsext");
    g_gpsQueryPending = true;
    g_gpsQueryTime = millis();
  }

  else if (clean.equalsIgnoreCase("getblestatus")) {
    char buf[128];
    snprintf(buf, sizeof(buf), "blestatus:%s,%s,%d",
             bleStateStr().c_str(), g_config.bmsMac, bmsConnected ? 1 : 0);
    sendSerial1(buf);
    Serial.print("[4G→DTU] ");
    Serial.println(buf);
  }

  else if (clean.startsWith("mode,")) {
    handleModeCommand(clean);
  }

  // ── DTU 响应处理 ──
  else if (clean.startsWith("config,csq,ok,")) {
    String val = clean.substring(14);
    val.trim();
    String resp = "csq:" + val;
    sendSerial1(resp);
    Serial.print("[4G→DTU] ");
    Serial.println(resp);
    // ★ 顺序查询: csq 响应后立即查询 GPS (避免两条命令连发导致 DTU 响应粘连)
    if (g_gpsQueryPending) {
      sendSerial1("config,get,gpsext");
    }
  }

  else if (clean.startsWith("config,gps,ok,")) {
    g_gpsQueryPending = false;
    // 参考 esp32s3.ino: config,gps,ok,<fix>,<lonDir>,<lon>,<latDir>,<lat>,<knots>,...
    // → gps:<fix>,<lonDir>,<lon>,<latDir>,<lat>,<speed km/h> (与服务端 parser.php 对齐)
    String val = clean.substring(14);
    int pos = 0, p[7] = {0, 0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 7; i++) {
      pos = val.indexOf(',', pos);
      if (pos < 0) break;
      p[i] = pos;
      pos++;
    }

    String prefix = val.substring(0, p[4]);
    float kmh = 0.0f;
    if (p[5] > 0 && p[6] > p[5]) {
      float knots = val.substring(p[5] + 1, p[6]).toFloat();
      kmh = knots * 1.852;
    }
    char buf[16];
    snprintf(buf, sizeof(buf), "%.2f", kmh);
    String resp = "gps:" + prefix + "," + String(buf);

    // 静止/运动判断 (v2.7, 全部模式生效): 速度≥3km/h 且 位移≥30m 才算运动 (双条件防漂移)
    float curLat = val.substring(p[3] + 1, p[4]).toFloat();
    float curLon = val.substring(p[1] + 1, p[2]).toFloat();
    bool speedMoving = (kmh >= G_GPS_MOVING_KMH);
    bool firstFix = (g_lastLat == 0.0f && g_lastLon == 0.0f);
    bool dispMoving = !firstFix && (haversineM(g_lastLat, g_lastLon, curLat, curLon) >= G_GPS_MOVE_DIST_M);
    if (speedMoving && (firstFix || dispMoving)) {
      g_moving = true;        // 确认真实移动 (位移≥30m 不可能由漂移产生)
      g_stillStart = 0;
    } else if (!speedMoving && !dispMoving) {
      g_moving = false;       // 速度与位移双静止 → 静止
      if (g_stillStart == 0) g_stillStart = millis();
    }
    // 混合状态(速度够但位移不足/反之): 保持上次状态, 防漂移单次误翻转
    // GPS 只看移动/静止 (v2.11, 网页开关无关): 移动→2s 实时, 静止→30s 本地探测(移动即恢复)
    g_gpsIntervalMs = g_moving ? g_realtimeGpsMs : g_trackGpsMs;
    bool stopGps = !g_moving && (millis() - g_stillStart >= G_STILL_STOP_MS);
    if (!stopGps) {
      g_lastLat = curLat;     // 仅上报时更新位移基准点
      g_lastLon = curLon;
      sendSerial1(resp);
      Serial.print("[4G→DTU] ");
      Serial.println(resp);
    } else {
      Serial.println("[省流] 静止≥60s, 暂停 GPS 上报 (车辆移动后自动恢复 2s 实时)");
    }
  }

  else {
    Serial.print("[SERIAL1 未识别] ");
    Serial.println(clean);
  }
}

// 接收 UART1 数据 (行缓冲, 参考 esp32s3.ino loop)
void handleSerial1Receive() {
  while (SERIAL1.available()) {
    char c = (char)SERIAL1.read();
    if (c == '\r' || c == '\n') continue;
    uart2ForwardBuffer += c;
    lastUartRxTime = millis();
  }

  if (uart2ForwardBuffer.length() > 0 && millis() - lastUartRxTime > 10) {
    uart2ForwardBuffer.trim();
    handleSerial1Command(uart2ForwardBuffer);
    uart2ForwardBuffer = "";
  }
}

// 双模式定时上报 (核心新增, millis 节流, 不阻塞 BLE/WebSocket)
void handleTimedReports() {
  unsigned long now = millis();

  // BMS 定时上报 (v2.12: 网页开→一律 2s 实时(充/放电/停放); 网页关→一律 30s)
  if (!bmsConnected) {
    lastRawFrameLen = 0;      // 断开时清空旧帧, 避免误报
    g_lastBmsReport = now;    // 重连后按新周期上报
  } else {
    // 网页开(realtime): 一律 2s 实时(充/放电/停放); 网页关(track): 一律 30s (v2.12)
    unsigned long bmsMs = g_reportMode.equals("realtime") ? g_realtimeBmsMs : g_trackBmsMs;
    if (now - g_lastBmsReport >= bmsMs) {
      g_lastBmsReport = now;
      // 距上次收帧超过 1 秒才发请求, 避免与 App/中继转发命令交错导致帧错乱
      if (millis() - lastDataTime > 1000) {
        sendCommand(CMD_CELL_INFO);
      }
      sendRawFrameToSERIAL1();
    }
  }

  // GPS/4G信号定时查询上报 (不依赖 BLE)
  // ★ 顺序查询避免粘连: 本周期先发 csq, DTU 回 config,csq,ok 时(handleSerial1Command)
  //   再发 gpsext → gps: 输出 → 同一周期内 csq 与 GPS 同步刷新
  //   若 csq 超时未响应(可能 DTU 不支持), 下一轮直接发 gpsext 兜底保证 GPS 数据
  if (now - g_lastGpsReport >= g_gpsIntervalMs) {
    g_lastGpsReport = now;
    if (!g_gpsQueryPending || now - g_gpsQueryTime > 10000) {
      if (g_gpsQueryPending) {
        // 上一轮 csq 超时未响应: 跳过 csq 直接查 GPS 兜底
        sendSerial1("config,get,gpsext");
      } else {
        sendSerial1("config,get,csq");
      }
      g_gpsQueryPending = true;
      g_gpsQueryTime = now;
    }
  }

  // 蓝牙状态主动上报 (每30秒): 固件只在收到 getblestatus 指令时才回, 但服务端不发该指令,
  // 若不主动上报, 网页蓝牙状态会一直显示服务端缓存里的旧数据(如部署时残留的模拟器 MAC)
  static unsigned long lastBleReport = 0;
  if (now - lastBleReport >= 30000) {
    lastBleReport = now;
    char buf[128];
    snprintf(buf, sizeof(buf), "blestatus:%s,%s,%d",
             bleStateStr().c_str(), g_config.bmsMac, bmsConnected ? 1 : 0);
    sendSerial1(buf);
    Serial.print("[4G→DTU] ");
    Serial.println(buf);
  }
}

// 两点球面距离 (米, Haversine) —— 静止/运动判断用 (v2.7)
float haversineM(float lat1, float lon1, float lat2, float lon2) {
  const float R = 6371000.0f;
  const float d2r = PI / 180.0f;
  float dLat = (lat2 - lat1) * d2r;
  float dLon = (lon2 - lon1) * d2r;
  float a = sin(dLat / 2) * sin(dLat / 2)
          + cos(lat1 * d2r) * cos(lat2 * d2r) * sin(dLon / 2) * sin(dLon / 2);
  return R * 2.0f * atan2(sqrt(a), sqrt(1.0f - a));
}

// 4G/GPS 串口主处理入口 (loop 中调用)
void handle4G() {
  handleSerial1Receive();
  flushPendingTx();    // ★ 发送排队的待发数据 (间隔保护, 非阻塞)
  handleTimedReports();
}

// ==================== 初始化 ====================

void initWiFiAP() {
  Serial.println("[WiFi] 启动 AP 模式...");

  // 1. 设置 WiFi 模式为 AP
  WiFi.mode(WIFI_AP);

  // 2. 降低 WiFi 发射功率 (减少与 BLE 的射频冲突, 参考代码策略)
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  // 3. 配置 AP 网络参数
  WiFi.softAPConfig(
    IPAddress(192, 168, 4, 1),
    IPAddress(192, 168, 4, 1),
    IPAddress(255, 255, 255, 0)
  );

  // 4. 启动 AP
  bool apStarted = WiFi.softAP(g_config.apSsid, g_config.apPassword, 1, 0, 8);

  if (apStarted) {
    // 5. WiFi 省电: 增大 beacon 间隔 + 允许 modem 休眠 (参考代码策略)
    //    beacon 从 100ms → 1000ms, 射频空闲时间 x10, 大幅减少与 BLE 冲突
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    wifi_config_t wifi_cfg;
    esp_wifi_get_config(WIFI_IF_AP, &wifi_cfg);
    wifi_cfg.ap.beacon_interval = 200;  // 200ms 平衡响应速度和省电
    esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg);

    IPAddress IP = WiFi.softAPIP();
    Serial.printf("[WiFi] AP 启动成功!\n");
    Serial.printf("[WiFi] SSID: %s\n", g_config.apSsid);
    Serial.printf("[WiFi] 密码: %s\n", g_config.apPassword);
    Serial.printf("[WiFi] IP:    %s\n", IP.toString().c_str());
    Serial.printf("[WiFi] 功率:  8.5dBm (降低冲突)\n");
    Serial.printf("[WiFi] 省电:  MIN_MODEM, beacon=200ms\n");
  } else {
    Serial.println("[WiFi] AP 启动失败! 重试...");
    delay(1000);
    WiFi.softAP(g_config.apSsid, g_config.apPassword, 1, 0, 8);
    Serial.println("[WiFi] AP 重试完成");
  }
}

void initWebServer() {
  // REST API 路由
  httpServer.on("/api/data", HTTP_GET, handleGetData);
  httpServer.on("/api/status", HTTP_GET, handleGetStatus);
  httpServer.on("/api/config", HTTP_GET, handleGetConfig);
  httpServer.on("/api/config", HTTP_POST, handlePostConfig);
  httpServer.on("/api/reconnect", HTTP_POST, handleReconnect);
  httpServer.on("/api/refresh", HTTP_POST, handleRefresh);
  httpServer.on("/api/reboot", HTTP_POST, handleReboot);
  httpServer.on("/api/wifi/off", HTTP_POST, handleWifiOff);
  httpServer.on("/api/wifi/on", HTTP_POST, handleWifiOn);
  // v9.17: 手动扫描 (Web 后台"扫描保护板")
  httpServer.on("/api/scan", HTTP_POST, handleScan);
  httpServer.on("/api/scan/status", HTTP_GET, handleScanStatus);
  httpServer.on("/api/scan/result", HTTP_GET, handleScanResult);

  // CORS 预检
  httpServer.on("/api/data", HTTP_OPTIONS, handleOptions);
  httpServer.on("/api/status", HTTP_OPTIONS, handleOptions);
  httpServer.on("/api/config", HTTP_OPTIONS, handleOptions);

  // 主页
  httpServer.on("/", HTTP_GET, handleRoot);

  httpServer.begin();
  Serial.println("[HTTP] Web 服务器已启动 (端口 80)");
}

void initWebSocket() {
  webSocketServer = WebSocketsServer(WS_PORT);
  webSocketServer.onEvent(webSocketEvent);
  webSocketServer.begin();
  Serial.println("[WS] WebSocket 服务器已启动 (端口 81)");
}

void loadConfig() {
  prefs.begin("gateway", true);
  String savedName = prefs.getString("relayName", "");
  if (savedName.length() > 0) {
    strncpy(g_config.relayName, savedName.c_str(), 31);
    g_config.relayName[31] = '\0';
  }
  String savedMac = prefs.getString("bmsMac", "");
  if (savedMac.length() > 0) {
    strncpy(g_config.bmsMac, savedMac.c_str(), 19);
    g_config.bmsMac[19] = '\0';
  } else {
    strncpy(g_config.bmsMac, DEFAULT_BMS_MAC, 19);
    g_config.bmsMac[19] = '\0';
  }
  String savedApSsid = prefs.getString("apSsid", "");
  if (savedApSsid.length() > 0) {
    strncpy(g_config.apSsid, savedApSsid.c_str(), 31);
    g_config.apSsid[31] = '\0';
  }
  String savedApPass = prefs.getString("apPassword", "");
  if (savedApPass.length() > 0) {
    strncpy(g_config.apPassword, savedApPass.c_str(), 63);
    g_config.apPassword[63] = '\0';
  }
  prefs.end();
  Serial.printf("[Config] 中继名称: %s\n", g_config.relayName);
  Serial.printf("[Config] BMS MAC: %s\n", g_config.bmsMac);
  Serial.printf("[Config] AP SSID: %s\n", g_config.apSsid);
}

// ★v2.14: 记录上次重启原因 (区分 定时重启/看门狗卡死/上电/崩溃, Web 后台「系统状态」可查)
void recordResetReason() {
  prefs.begin("gateway", false);
  String lastReason = prefs.getString("lastRebootReason", "");
  prefs.remove("lastRebootReason");
  esp_reset_reason_t r = esp_reset_reason();
  const char* t = "未知";
  switch (r) {
    case ESP_RST_POWERON:   t = "上电启动"; break;
    case ESP_RST_SW:        t = "软件重启"; break;
    case ESP_RST_PANIC:     t = "异常崩溃"; break;
    case ESP_RST_TASK_WDT:  t = "任务看门狗(卡死)"; break;
    case ESP_RST_WDT:       t = "看门狗(卡死)"; break;
    case ESP_RST_INT_WDT:   t = "中断看门狗(卡死)"; break;
    case ESP_RST_BROWNOUT:  t = "电压跌落"; break;
    default: break;
  }
  String info;
  if (r == ESP_RST_SW && lastReason.length() > 0) {
    info = "上次重启: " + lastReason;      // 自愈/定时/手动重启前存的真实原因
  } else {
    info = String("上次重启: ") + t;
  }
  prefs.putString("lastResetInfo", info);
  prefs.end();
  Serial.printf("[BMS] %s\n", info.c_str());
}

// ==================== 主程序 ====================

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println("========================================");
  Serial.println(" ESP32-S3 JK-BMS BLE 中继网关 + 4G/GPS");
  Serial.println("========================================");
  Serial.printf("目标 BMS: %s\n", g_config.bmsMac);
  Serial.println();

  // ★v2.14: 记录上次重启原因 (在 prefs 读写前先于 loadConfig 调用无影响, 独立 namespace 段)
  recordResetReason();

  // ★ 自愈: 任务看门狗 30s (ESP32 卡死自动重启)
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&wdtCfg);
  esp_task_wdt_add(NULL);

  // ★ 自愈: DTU 电源控制引脚 (冷重启用, 默认 -1 不启用)
  if (DTU_POWER_PIN >= 0) {
    pinMode(DTU_POWER_PIN, OUTPUT);
    digitalWrite(DTU_POWER_PIN, HIGH);
  }

  // 初始化 4G/GPS 串口 (UART1: TX=13→DTU RXD, RX=12←DTU TXD, 接银尔达 M100PG-DTU)
  // 加大发送缓冲, 避免 600 字符的 b:r,<hex> 长帧阻塞主循环
  SERIAL1.setTxBufferSize(2048);
  SERIAL1.begin(115200, SERIAL_8N1, SERIAL1_RX_PIN, SERIAL1_TX_PIN);
  Serial.println("[4G] UART1 已初始化 (115200, 接银尔达 M100PG-DTU)");

  // 初始化 NVS
  loadConfig();

  // 初始化 BOOT 按键 (GPIO0)
  pinMode(BOOT_BTN_PIN, INPUT_PULLUP);

  // 先初始化 WiFi AP (必须在 BLE 之前, 避免射频冲突)
  initWiFiAP();
  g_lastWebRequest = millis();

  // 初始化 Web 服务器
  initWebServer();

  // 初始化 WebSocket
  initWebSocket();

  // 再初始化 BLE (WiFi 之后)
  NimBLEDevice::init(g_config.relayName);
  NimBLEDevice::setPower(3);  // +9dBm, 确保BLE信号强度
  // ★FIX-4 (完整版): MTU 上限提到 517 (sdkconfig 默认 CONFIG_BT_NIMBLE_ATT_MTU_MAX=517)。
  //   iOS 协商到 185、安卓可到 517、BMS 到 185。300 字节帧: iOS 收 2 块、安卓收 1 块,
  //   与真实 BMS 的大块通知行为一致, 不再是 15 个 20 字节小块。
  NimBLEDevice::setMTU(517);

  // 初始化 BLE 服务端 (完整模拟 JK-BMS)
  initBleServer();

  // 连接 BMS (非阻塞: 启动异步扫描, loop 中自动完成连接)
  Serial.println("[BMS] 启动异步扫描连接...");
  lastReconnectAttempt = 0;  // 允许立即开始扫描
  startBmsConnect();

  Serial.println();
  Serial.println("系统就绪!");
  Serial.println("[BLE-Server] ESP32 正在广播, 极空 App 可搜索连接");
  Serial.printf("Web 后台: http://%s\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("API:      http://%s/api/data\n", WiFi.softAPIP().toString().c_str());
  Serial.printf("WS:       ws://%s:81\n", WiFi.softAPIP().toString().c_str());
  Serial.println();
}

void loop() {
  // ★ 自愈: 喂任务看门狗 (loop 正常执行即保持存活)
  esp_task_wdt_reset();

  // 0. BOOT 按键检测 (长按5秒重新开启 WiFi)
  if (digitalRead(BOOT_BTN_PIN) == LOW) {
    if (!g_btnPressed) {
      g_btnPressed = true;
      g_btnPressStart = millis();
    } else if (millis() - g_btnPressStart >= 5000) {
      Serial.println("[BTN] BOOT 键长按5秒, 重启设备恢复 WiFi...");
      ESP.restart();
    }
  } else {
    g_btnPressed = false;
  }

  // 0.5 BLE 广播健康检查 (v9.41/v9.42: BMS 未连接时对客户端隐身, 防抢连挤占 MTU 协商)
  static unsigned long lastAdvCheck = 0;
  if (pBleServer && millis() - lastAdvCheck > 5000) {
    lastAdvCheck = millis();
    NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();

    // v9.41: BMS 未连接时停止广播 —— 防止客户端 (显示屏/极空 App) 先抢连中继,
    //   挤占 BMS 连接资源导致 MTU 协商失败 (MTU=23) / reason 520。
    //   BMS 连上后恢复广播, 客户端即可正常连接取数。
    if (!bmsConnected) {
      if (pAdv->isAdvertising()) {
        Serial.println("[BLE-Server] BMS 未连接, 停止广播 (防客户端抢连)");
        pAdv->stop();
      }
      // v9.42: 同时主动断开已连接的客户端 (仅停广播不会断开已建立连接,
      //   旧客户端仍占用 Server 侧连接槽 → BMS 重连时 MTU 协商照样被挤)。
      for (int i = 0; i < MAX_BLE_CLIENTS; i++) {
        if (g_connStates[i].active) {
          uint16_t h = g_connStates[i].connHandle;
          Serial.printf("[BLE-Server] BMS 未连接, 断开客户端 handle=%d (释放 Server 侧资源)\n", h);
          if (pBleServer) pBleServer->disconnect(h);
        }
      }
    } else {
      // BMS 已连接: 未达上限保持广播
      if (g_bleClientCount < MAX_BLE_CLIENTS && !pAdv->isAdvertising()) {
        Serial.println("[BLE-Server] BMS 已连接, 广播恢复");
        pAdv->start();
      } else if (g_bleClientCount >= MAX_BLE_CLIENTS && pAdv->isAdvertising()) {
        Serial.println("[BLE-Server] 已达最大连接数, 停止广播");
        pAdv->stop();
      }
    }
  }

  // 1. 始终优先处理网络请求 (HTTP + WebSocket)
  if (g_wifiEnabled) {
    httpServer.handleClient();
    webSocketServer.loop();

    // v9.33: WiFi 自动关闭已禁用 —— 中继场景 WiFi 必须常开 (App/显示屏/后台随时可能访问),
    //   10 分钟无操作就关 WiFi 导致"web 打不开" (用户隔一段时间再看就关掉了, 多次误判为设备卡死)
    // if (millis() - g_lastWebRequest > WIFI_AUTO_OFF_MS) { ... }
  }

  // 1.5 ★ 4G/GPS 串口处理: 接收 DTU 下行指令 + 双模式定时上报 (millis 节流, 不阻塞 BLE/WebSocket)
  handle4G();

  // 2. BLE 连接管理 (非阻塞: 异步扫描 → 扫到后连接)
  loopScanWork();  // v9.33: 手动扫描执行器 (loop 上下文, 安全调用 NimBLE)
  if (!bmsConnected) {
    if (g_doConnect) {
      g_doConnect = false;
      if (g_wifiEnabled) {
        httpServer.handleClient();
        webSocketServer.loop();
      }
      connectToBms();
      if (g_wifiEnabled) {
        httpServer.handleClient();
        webSocketServer.loop();
      }
    } else {
      startBmsConnect();
    }
    yield();
    return;
  }

  // 3. 定时请求 BMS 数据 (完整版 FIX-2: 无订阅客户端时 5s 轮询保持 Web/4G 数据新鲜;
  //    有订阅客户端时 30s 低频兜底, 让 App 自己驱动请求, 避免响应交错)
  unsigned long pollInterval = (g_subscribedConnCount > 0) ? IDLE_POLL_INTERVAL : REQUEST_INTERVAL;
  if (millis() - lastRequest > pollInterval) {
    sendCommand(CMD_CELL_INFO);
    lastRequest = millis();
  }

  // 4. 数据超时检测 (30秒无数据则重连)
  if (millis() - lastDataTime > DATA_TIMEOUT) {
    Serial.println("[BMS] 数据超时, 重连...");
    if (pClient) {
      pClient->disconnect();
      NimBLEDevice::deleteClient(pClient);  // v9.5/FIX-22: 超时断连也销毁, 防止下次复用失效对象
      pClient = nullptr;
    }
    bmsConnected = false;
    g_advDevice = nullptr;
    frameBuffer.clear();
    lastReconnectAttempt = 0;
    return;
  }
  // 5. 自愈检查 (每 30 秒; 启动 2 分钟宽限)
  static unsigned long lastSelfHealCheck = 0;
  if (millis() - lastSelfHealCheck > 30000) {
    lastSelfHealCheck = millis();
    unsigned long now = millis();
    if (now > STARTUP_GRACE) {
      // ① DTU 串口无响应 (任何串口行都没有) → 重启
      if (lastDtuResponse == 0 || now - lastDtuResponse > DTU_RESPONSE_TIMEOUT) {
        selfHealReboot("DTU 串口无响应超过 8 分钟");
      }
      // ② MQTT 下行静默断开: 仅在"曾经收到过下行指令"后启用检查,
      //    避免 网页关闭 + 未配 cron(本就无下行) 的场景误重启
      if (lastDownlink != 0 && now - lastDownlink > DOWNLINK_TIMEOUT) {
        selfHealReboot("EMQX 下行无响应超过 12 分钟 (MQTT 可能静默断开)");
      }
      // ③ 定时自动重启 (不依赖下行, 电摩无插座场景的兜底)
      if (AUTO_REBOOT_HOURS > 0 && now > AUTO_REBOOT_HOURS * 3600UL * 1000UL) {
        char rebootReason[64];
        snprintf(rebootReason, sizeof(rebootReason), "定时自动重启 (连续运行满 %d 小时)", AUTO_REBOOT_HOURS);
        selfHealReboot(rebootReason);
      }
    }
  }

  // 6. 短延时
  delay(20);
}

// ★ 自愈: 重启前可选冷重启 DTU (断电 3s 再上电), 然后 ESP32 重启
void selfHealReboot(const char* reason) {
  Serial.printf("[自愈] 触发重启, 原因: %s\n", reason);
  // ★v2.14: 重启前记录原因, 重启后 Web 后台可查
  prefs.begin("gateway", false);
  prefs.putString("lastRebootReason", String(reason));
  prefs.end();
  if (DTU_POWER_PIN >= 0) {
    Serial.println("[自愈] 冷重启 DTU (断电 3s 再上电)...");
    digitalWrite(DTU_POWER_PIN, LOW);
    delay(3000);
    digitalWrite(DTU_POWER_PIN, HIGH);
    delay(2000);
  }
  ESP.restart();
}
