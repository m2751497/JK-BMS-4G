'use strict';
/* mqtt-client.js — MQTT 长连接 (常驻, 订阅上行 T / 发布下行 R) */
const mqtt = require('mqtt');

let client = null;
let cfg = null;
let lastConnectedAt = 0;

function connect(mqttCfg, onUplink, onStateChange) {
    cfg = mqttCfg;
    const url = `${cfg.tls ? 'mqtts' : 'mqtt'}://${cfg.host}:${cfg.port}`;
    const opts = {
        username: cfg.username,
        password: cfg.password,
        keepalive: cfg.keepalive ?? 60,
        clean: cfg.clean !== false,          // 长连接必须 clean:true, 避免堆积持久会话计费
        reconnectPeriod: cfg.reconnectPeriod ?? 5000,
        connectTimeout: 10000,
        rejectUnauthorized: false,           // Serverless 证书兼容
    };
    if (cfg.ca) opts.ca = cfg.ca;
    client = mqtt.connect(url, opts);

    client.on('connect', () => {
        lastConnectedAt = Date.now();
        console.log(`[MQTT] ✅ 已连接 ${cfg.host}:${cfg.port} (clean=${opts.clean})`);
        client.subscribe(cfg.topic, (err) => {
            if (err) console.error(`[MQTT] ❌ 订阅 ${cfg.topic} 失败:`, err.message);
            else console.log(`[MQTT] 已订阅上行主题: ${cfg.topic}`);
        });
        onStateChange?.(true);
    });
    client.on('message', (topic, payload) => {
        const txt = payload.toString().trim();
        if (txt) onUplink?.(txt, topic);
    });
    client.on('reconnect', () => console.log('[MQTT] 重连中...'));
    client.on('error', (err) => console.error('[MQTT] 错误:', err.message));
    client.on('close', () => { console.log('[MQTT] 连接关闭'); onStateChange?.(false); });
    client.on('offline', () => { console.log('[MQTT] 离线'); onStateChange?.(false); });
}

/** 下行: 网页触发 → 发布到 controlTopic (R), 设备即时收到 */
function publishControl(msg) {
    if (!client || !client.connected) {
        console.error('[MQTT] 下行失败: 未连接');
        return false;
    }
    client.publish(cfg.controlTopic, String(msg), { qos: 0 });
    console.log(`[MQTT] 下行 → ${cfg.controlTopic}: ${msg}`);
    return true;
}

function isConnected() { return client ? client.connected : false; }
function getLastConnectedAt() { return lastConnectedAt; }

module.exports = { connect, publishControl, isConnected, getLastConnectedAt };
