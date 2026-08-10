# JK-BMS 4G 网关 — Node.js 常驻进程版

用 **MQTT 长连接 + SQLite + Express** 替代原 PHP WebHook 架构，单进程常驻（PM2 守护），下行指令网页点按钮即时送达设备。

```
Node.js 常驻进程 (PM2 守护)
├─ mqtt 客户端: 长连接订阅 T(上行) / 发布 R(下行)
├─ 上行消息 → better-sqlite3 写入 → 内存缓存(网页 API 秒读)
├─ 下行: 网页点按钮 → mqtt.publish(R 主题) → 设备即时收到
└─ express: 提供网页 + REST API(替代宝塔里的 PHP 站点)
```

## 功能（对齐原 PHP 版，全部保留）
- **上行解析**：`b:r` BMS 帧（0x01/0x02，24S/32S 自动识别 + CRC）、`gps`、`csq`、`blestatus`、`lbs`、`switch`、`sendfreq`、`autobal`、`relay`
- **下行 mode**：网页开/关 + 停在 BMS 页自动切换 realtime/track 上报频率（`mode,realtime/track,<bms_ms>,<gps_ms>` 发到 R 主题）
- **业务**：里程随充随学、容量校准学习、告警（过压/欠压/压差/温度/SOC，5 分钟去重）、充电循环统计、轨迹（按天/行程/回放，WGS84→GCJ02）
- **鉴权**：登录（scrypt + session cookie 7 天 + IP 5 次/5 分钟锁定），网页/API 全站保护
- **两种网页**：`/`（深色，原 index.php 风格）/ `/dashboard.html`（浅色卡片）
- **数据保留**：bms/gps 数据每日自动清理（默认 60 天）

## 部署（Linux VPS / 云服务器）

### 1. 环境
```bash
# 安装 Node.js 18+ (以 NodeSource 20 LTS 为例)
curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
apt install -y nodejs

# 安装 PM2 (进程守护, 开机自启)
npm install -g pm2
```

### 2. 安装依赖
```bash
cd 02-Node服务端
npm install --omit=dev        # better-sqlite3 有预编译, 无需编译工具链
```

### 3. 配置（脱敏模板）
```bash
cp config.example.json config.json
nano config.json
```
必填项：
| 字段 | 说明 |
|---|---|
| `mqtt.host/port` | EMQX 地址与 TLS 端口（如 `f0906bb6.ala.cn-hangzhou.emqxsl.cn:8883`） |
| `mqtt.username/password` | EMQX「客户端认证」账号密码 |
| `mqtt.ingestToken` | HTTP 兼容上报令牌（任意随机长串） |
| `auth.username/password` | 网页登录账号密码 |
| `settings.amapKey` | 高德 JS API Key（看板地图用，可留空） |

### 4. 启动（PM2 守护）
```bash
pm2 start server.js --name jk-bms
pm2 save
pm2 startup          # 按提示执行输出的命令, 实现开机自启
pm2 logs jk-bms      # 看日志
```

### 5. 反向代理（可选, 域名+HTTPS）
Nginx 示例：
```nginx
server {
    listen 443 ssl;
    server_name jkbms.你的域名.com;
    ssl_certificate     /path/fullchain.pem;
    ssl_certificate_key /path/privkey.pem;
    location / {
        proxy_pass http://127.0.0.1:1880;
        proxy_set_header Host $host;
        proxy_set_header X-Forwarded-For $remote_addr;
        # WebSocket 支持
        proxy_http_version 1.1;
        proxy_set_header Upgrade $http_upgrade;
        proxy_set_header Connection "upgrade";
    }
}
```

## 与设备对接
- **MQTT 长连接替代 WebHook**：设备（DTU/固件）上行发到 `T` 主题，下行订阅 `R` 主题 —— 与 EMQX 配置完全一致，**设备端零改动**
- 兼容 HTTP 直报（测试/调试用）：`POST /api/ingest`，头 `X-Ingest-Token: <ingestToken>`，body 为文本（如 `b:r,<hex>` / `gps:...`）

## 常用 API（网页前端同款）
| 方法 | 路径 | 说明 |
|---|---|---|
| POST | `/api/login` | 登录（body: `{username,password}`） |
| GET | `/api/latest` | 最新状态（BMS/GPS/状态/续航） |
| GET | `/api/track?from=&to=` | 轨迹（毫秒时间戳，默认今日） |
| GET | `/api/track/dates` | 有轨迹的日期 |
| GET | `/api/trips?date=2026-08-10` | 行程统计 |
| GET | `/api/replay?date=` | 轨迹回放 |
| GET/POST | `/api/range` | 里程基数（POST: `{rangeFactor,learningEnabled}`） |
| GET | `/api/alarms?hours=24` | 告警 |
| GET | `/api/chargecycles?days=30` | 充电循环 |
| GET/POST | `/api/capacity` | 容量校准（POST: `{enabled}`） |
| GET/POST | `/api/settings` | 设置读写 |
| POST | `/api/mode` | 手动切上报模式（`{mode:'realtime'\|'track'}`） |
| WS | `/ws/bms` | 实时推送（新 BMS/GPS 帧） |

> 兼容原 PHP 的 `?action=` 风格：`GET /api?action=latest` 等，返回结构一致，旧前端可直接改 `const API='/api'` 复用。

## 测试自检
```bash
# 模拟设备上行（替换令牌）
curl -X POST -H "X-Ingest-Token: 你的令牌" --data-binary "csq:25" http://127.0.0.1:1880/api/ingest
curl -X POST -H "X-Ingest-Token: 你的令牌" -d "gps:1,E,116.397128,N,39.916527,25.5" http://127.0.0.1:1880/api/ingest
# 查状态
curl http://127.0.0.1:1880/api/latest
```

## 迁移说明（从 PHP 版切换）
1. 保留原 PHP 站点不动，先用本目录在 VPS 起 Node 服务（设备 EMQX 配置不变）
2. 浏览器打开 Node 服务域名验证（登录 → 数据 → 轨迹）
3. 验证通过后，把设备 EMQX 回调/宝塔 WebHook 停掉，DNS 切到 Node 服务
4. 原 PHP 站点可下线（数据表结构一致，如需历史数据可导出 SQLite 导入）
