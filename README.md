# JK BMS 4G 远程监控系统

用最简单可靠的架构实现锂电池（极空/JK 保护板）BMS 远程监控：

- **ESP32-S3** 通过 BLE 读取极空保护板实时数据，同时可作 BLE 中继（极空 App 可并行接入）
- 经**银尔达 M100PG-DTU（4G + GPS）**通过 MQTT 上传
- **PHP 服务端**（WebHook 架构，无常驻进程，纯虚拟主机即可部署）接收、解析、存储、展示
- 网页打开时 GPS/BMS 实时刷新，关闭后自动降频省流量，轨迹照常记录可回放

<p align="center">
  <img src="docs/screenshots/gps.png" width="48%" alt="GPS 实时定位">
  <img src="docs/screenshots/bms.png" width="48%" alt="BMS 电池详情">
  <img src="docs/screenshots/track.png" width="48%" alt="历史轨迹回放">
  <img src="docs/screenshots/settings.png" width="48%" alt="设置页">
</p>

```
极空 BMS ──BLE──> ESP32-S3 ──UART──> 银尔达 M100PG-DTU ──MQTT──> EMQX
  ▲                                                                    │ 规则引擎
  └───── BLE 中继（极空 App 可接入）                                    ▼
                                                              api.php?action=ingest
                                                                   （PHP + SQLite）
                                                              ▲
                                                              └── 网页轮询 / 心跳
```

## 目录结构

```
01-网页服务端/        PHP 服务端（index 看板 / api 接口 / ingest 接收 / parser 解析 /
                    db SQLite / range 里程学习 / login+auth 登录鉴权 / config 配置）
02-ESP32固件/        esp32s3-gateway.ino（Arduino 固件，需 NimBLE-Arduino 库）+ build/ 编译产物
03-参考资料/         第三方参考代码（不随仓库发布）
计划书.md              架构 / 硬件接线 / 上报规则 / 数据流 / 服务端设计（按现状整理）
部署清单.md          部署步骤与自检清单（上传前必读）
```

## 快速开始（服务端）

1. 把 `01-网页服务端/` 全部文件上传到虚拟主机网站根目录（PHP ≥ 7.4，需 `pdo_sqlite` / `curl` / `openssl`）
2. 修改 `config.php`：MQTT 直发凭据（EMQX「客户端认证」账号）/ WebHook token / 登录密码
3. EMQX 控制台建转发规则：`SELECT payload FROM "T"` → HTTP 服务连接器 → `http://你的域名/api.php?action=ingest`，请求头带 `X-Ingest-Token`
4. 浏览器打开网页登录，设备上报后即显示数据

详细步骤见 [`部署清单.md`](部署清单.md)。

## 固件

`02-ESP32固件/esp32s3-gateway/esp32s3-gateway.ino`（v2.21，**适配 ESP32-S3 Super Mini 4MB Flash**），Arduino IDE 编译，所需库：
- Arduino-ESP32 Core ≥ 3.0.0
- NimBLE-Arduino ≥ 2.2.2（双 FFE1 必须用 NimBLE，勿用 Bluedroid）
- WebSockets / ArduinoJson

**编译要点（4MB Flash + OTA）**：Tools → Board 选 `ESP32S3 Dev Module`；Partition Scheme 选 **Custom**（用 `02-ESP32固件/esp32s3-gateway/partitions.csv` 自定义分区：双 APP 各 1.94MB，支持 OTA）；Flash Size 选 **4MB**；**PSRAM 选 `QSPI PSRAM`**（实测 2MB PSRAM 为 QSPI 模式）；USB CDC On Boot=Enabled（原生 USB 口出串口日志）

**固件功能**（BLE 基于「中继完整版 v9.60」，含 4G/GPS/OTA）：
- **BLE 读取+中继**：帧级转发（完整帧 + CRC 校验通过才转发，坏帧自愈对齐）；多客户端（最多 5 个）按各自 MTU 分块推送（150B 块模拟真实 BMS）；AT 心跳 / 命令帧回显原样透传（防 App 超时断开）
- **稳定性**：ble_svc_gap_init 崩溃根治（setup 即广播保持 active，v9.57）；回调内不查 NimBLE API / RSSI 5s 限流 / 自维护计数（v9.33/34）；断连销毁重建客户端（FIX-22）；MTU<100 连接上补协商 exchangeMTU（v9.60）
- **防抢连**：BMS 未连接时 onConnect 直接拒绝客户端（v9.57）；5 客户端连满停广播、断开自动恢复（v9.59）
- **兼容性**：特征选择"属性驱动 + handle 优先"，兼容 V20S 双 FFE1 与其他 handle 布局；协议模式（JK02_24S/JK02_32S/JK04）与电芯帧偏移 Web 后台可配
- **GPS 上报**：只看移动/静止（与网页开关无关）——移动 2s 实时、静止 ≥60s 停发（30s 本地探测，动即恢复）；速度 ≥3km/h 且位移 ≥30m 双条件判运动（防漂移）
- **BMS 上报（按电流+按页面）**：仅网页停在「BMS」页才 2s 高频；其他页/网页关按电流——充/放电（|I|≥0.3A）30s 上报（供续航学习），停放停报零流量
- **双模式控制**：下行指令 **MQTT 直连发布**（EMQX 客户端认证账号，TLS 8883），网页所在页面自动切换上报频率
- **OTA 固件升级**：Web 后台「固件升级」上传 `.bin` 直接烧写，成功自动重启、失败自动回滚（4MB Flash 双 OTA 分区）；首次烧录用 `firmware/*.merged.bin` 全量刷
- **自愈机制**：卡死看门狗 30s 重启 / DTU 串口无响应 8 分钟重启 / MQTT 下行静默断 12 分钟重启；**重启原因记录**：Web 后台「系统状态」可查上次重启原因

> 编译产物见 `02-ESP32固件/build/`（该目录不入仓库）；**历史版本源码归档**见 `02-ESP32固件/versions/`（v2.14~v2.21）
## 服务端功能

- **WebHook 接收**：EMQX 规则转发，无常驻进程，纯虚拟主机可部署
- **网页看板**：GPS 实时定位（高德地图）、BMS 电池详情、单体电压/内阻、续航估算、天气、4G 信号、蓝牙状态
- **轨迹**：按天行程拆分、每日统计（按天查询+全部累计）、地图轨迹回放（1x~8x 倍速动画）
- **里程学习**：随充随学——每次充电结束记录基准，下次充电时用消耗电量与轨迹里程自动修正基数（不要求充满）；学习状态与历史记录在设置页实时显示
- **告警 / 充电循环 / 容量校准（融合 Node 版）**：单体过压/欠压/压差/温度/SOC 阈值告警（5 分钟去重）；充电周期记录（Ah/SOC）；满充→放电自动校准总容量（加权更新）
- **安全**：登录鉴权（bcrypt + 限速）、内部文件防直接访问、CSRF 防护、data/ 目录保护

## 安全提示

- 仓库中的 `config.php` 已**脱敏为占位符**，部署前必须填入自己的值；**勿把真实账号密码提交到公开仓库**（可用 `git update-index --skip-worktree "01-网页服务端/config.php"` 防止误提交）
- 登录密码请在网页「设置」页修改（bcrypt 哈希存储），登录失败 5 次自动锁定 5 分钟
- 内部 PHP 文件均有防直接访问守卫（需经入口文件加载，Nginx/Apache 均生效）
- 高德 JS API Key 请在开放平台绑定域名白名单

## 协议

仅供学习参考，使用风险自负。
