'use strict';
/* server.js — JK-BMS 4G 网关 Node.js 常驻进程版 (PM2 守护)
 *   ├─ MQTT 长连接: 订阅 T(上行) / 发布 R(下行)
 *   ├─ 上行消息 → better-sqlite3 写入 → 内存缓存(网页 API 秒读)
 *   ├─ 下行: 网页点按钮 → mqtt.publish(R 主题) → 设备即时收到
 *   └─ express: 提供网页 + REST API (替代宝塔里的 PHP 站点)
 * 运行: node server.js  (配置复制 config.example.json → config.json 并填真实凭据)
 */
const fs = require('fs');
const path = require('path');
const express = require('express');
const http = require('http');
const WebSocket = require('ws');

const db = require('./lib/db');
const parser = require('./lib/parser');
const insights = require('./lib/insights');
const mqttClient = require('./lib/mqtt-client');

/* ==================== 配置加载 ==================== */
function loadConfig() {
    const cfgPath = path.join(__dirname, 'config.json');
    if (!fs.existsSync(cfgPath)) {
        fs.copyFileSync(path.join(__dirname, 'config.example.json'), cfgPath);
        console.log('⚠ 已生成 config.json (模板), 请填入真实 MQTT 凭据后重启');
    }
    return JSON.parse(fs.readFileSync(cfgPath, 'utf8'));
}
const config = loadConfig();

/* ==================== 数据库 ==================== */
db.open(config.db.path);

/* ==================== 日志(内存环形 + 可选文件) ==================== */
const logBuf = [];
function ingestLog(msg) {
    const line = `[${new Date().toLocaleString('zh-CN', { hour12: false })}] ${msg}`;
    logBuf.push(line);
    if (logBuf.length > 200) logBuf.shift();
    console.log(line);
}

/* ==================== 内存状态 ==================== */
const online = new Map();   // uid -> { lastSeen, page }

function onlineCount() { return online.size; }
function onlineNow() {
    const cutoff = Date.now() / 1000 - config.settings.onlineTimeout;
    for (const [uid, v] of online) if (v.lastSeen < cutoff) online.delete(uid);
    return online.size;
}

/* ==================== 上行处理 (MQTT 消息 → 解析 → 入库 → 业务) ==================== */
function handleUplink(txt) {
    const { type, data } = parser.parseTextMessage(txt);
    const now = Date.now();
    db.cache.lastAnyTs = now;

    if (type === 'bms' && data) {
        handleBMSData(data);
    } else if (type === 'gps' && data) {
        handleGPSData(data);
    } else if (type === 'jzdw' && data) {
        db.cache.gps = { ...data, fix: true, ts: now };
        db.cache.lastGpsTs = now;
    } else if (type === 'csq' && data) {
        db.cache.status.csq = data.csq;
    } else if (type === 'blestatus' && data) {
        db.cache.status.bleState = data.state;
        db.cache.status.bleMac = data.mac;
        db.cache.status.bleConnected = data.connected;
        ingestLog(`蓝牙状态: ${data.state} (${data.mac})`);
    } else if (type === 'switch' && data) {
        db.cache.status[data.key] = data.enabled;
    } else if (type === 'relay' && data) {
        db.cache.status.relay = data.enabled;
    } else if (type === 'sendfreq' && data) {
        db.cache.status.sendfreq = data;
    } else if (type === 'autobal' && data) {
        db.cache.status.autobal = { ...(db.cache.status.autobal || {}), ...data };
    }
}

function handleBMSData(data) {
    const now = Date.now();
    if (data.frameType === 0x02) {
        // 0x02 电芯帧: 全字段入库 + 缓存 + 业务
        db.get().prepare(`INSERT INTO bms_data (ts,totalVoltage,current,power,soc,soh,capRemain,capNominal,cycleCount,temp1,temp2,mosTemp,cellCount,cellVoltages,cellResist,chargeMOS,dischargeMOS,balancerOn)
            VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`).run(
            now, data.totalVoltage, data.current, data.power, data.soc, data.soh,
            data.capRemain, data.capNominal, data.cycleCount, data.temp1, data.temp2, data.mosTemp,
            data.cellCount, JSON.stringify(data.cellVoltages), JSON.stringify(data.cellResist),
            data.chargeMOS ? 1 : 0, data.dischargeMOS ? 1 : 0, data.balancerOn ? 1 : 0);
        db.cache.bms = data;
        db.cache.lastBmsTs = now;
        insights.checkAlarms(data, config, ingestLog);
        insights.checkChargeCycle(data, config, ingestLog);
        insights.updateCapacityLearning(data, config, ingestLog);
        insights.updateRangeLearning(data, config, ingestLog);
    } else if (data.frameType === 0x01) {
        // 0x01 设置帧: 只更新额定容量, 不覆盖 MOS/均衡 (0x01 帧不含这些字段)
        if (data.capNominalValid) db.setSetting('capNominal', String(data.capNominal));
    }
    broadcast({ type: 'bms', data });
}

function handleGPSData(data) {
    const now = Date.now();
    db.cache.gps = { ...data, ts: now };
    db.cache.lastGpsTs = now;
    if (data.fix) {
        db.get().prepare('INSERT INTO gps_data (ts, type, lon, lat, speed, fix) VALUES (?,?,?,?,?,?)')
            .run(now, 'gps', data.lon, data.lat, data.speed, 1);
    }
    broadcast({ type: 'gps', data });
}

/* ==================== 下行 mode (网页开/关 + 页面切换) ==================== */
function buildModeCmd(mode) {
    const s = config.settings;
    const bmsMs = mode === 'realtime' ? (s.realtimeBmsSec ?? 6) * 1000 : 0;
    const gpsMs = mode === 'realtime' ? (s.realtimeGpsSec ?? 2) * 1000 : (s.trackGpsSec ?? 30) * 1000;
    return `mode,${mode},${bmsMs},${gpsMs}`;
}
function sendMode(mode) {
    const ok = mqttClient.publishControl(buildModeCmd(mode));
    if (ok) ingestLog(`[mode] 下发 ${mode}`);
    else console.log(`[mode] ⚠ 下发失败(${mode}), MQTT 未连接 (静默)`);
    return ok;
}
/* 网页 heartbeat/页面切换 → 决定模式: 在线且停在 bms 页 → realtime, 否则 track */
function refreshMode() {
    const n = onlineNow();
    const page = db.getPage();
    if (n > 0 && page === 'bms') sendMode('realtime');
    else if (n > 0) sendMode('track');
    else sendMode('track');
    return { online: n, page, mode: (n > 0 && page === 'bms') ? 'realtime' : 'track' };
}

/* ==================== WebSocket 广播 ==================== */
const wss = new WebSocket.Server({ noServer: true });
function broadcast(msg) {
    const s = JSON.stringify(msg);
    for (const ws of wss.clients) if (ws.readyState === WebSocket.OPEN) ws.send(s);
}

/* ==================== HTTP ==================== */
const app = express();
const server = http.createServer(app);
app.use(express.json({ limit: '1mb' }));
app.use(express.static(path.join(__dirname, 'public')));

require('./lib/api')(app, { config, db, parser, insights, mqttClient, ingestLog, online, onlineNow, refreshMode, sendMode, broadcast, handleUplink });

server.on('upgrade', (req, socket, head) => {
    if (req.url === '/ws/bms') wss.handleUpgrade(req, socket, head, (ws) => wss.emit('connection', ws));
});
wss.on('connection', (ws) => {
    ws.send(JSON.stringify({ type: 'hello' }));
});

/* ==================== 定时任务 ==================== */
setInterval(() => {
    const n = onlineNow();
    const page = db.getPage();
    // 在线状态变化: 0→多 发 realtime, 多→0 发 track (简化为每次刷新当前模式)
    refreshMode();
}, 15000);
setInterval(() => db.cleanupOldData(config.settings.dataRetentionDays ?? 60), 3600 * 1000);

/* ==================== 启动 ==================== */
server.listen(config.port, config.host, () => {
    console.log(`[HTTP] ✅ 服务已启动: http://${config.host}:${config.port}`);
    console.log(`[MQTT] 连接 ${config.mqtt.host}:${config.mqtt.port} 主题 ${config.mqtt.topic}/${config.mqtt.controlTopic}`);
    console.log(`[提示] 网页: http://127.0.0.1:${config.port}/  浅色版: /dashboard.html`);
});

mqttClient.connect(config.mqtt, handleUplink, (up) => {
    if (up) refreshMode();
});

process.on('uncaughtException', (err) => {
    console.error('[FATAL] 未捕获异常:', err);
    ingestLog(`[FATAL] ${err.message}`);
});
