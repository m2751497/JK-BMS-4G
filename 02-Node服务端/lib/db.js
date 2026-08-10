'use strict';
/* db.js — better-sqlite3 封装: 建表 + 公共读写 (表结构对齐 PHP 版) */
const path = require('path');
const fs = require('fs');
const Database = require('better-sqlite3');

let db = null;

const SCHEMA = `
CREATE TABLE IF NOT EXISTS bms_data (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    totalVoltage REAL,
    current REAL,
    power REAL,
    soc INTEGER,
    soh INTEGER,
    capRemain REAL,
    capNominal REAL,
    cycleCount INTEGER,
    temp1 REAL,
    temp2 REAL,
    mosTemp REAL,
    cellCount INTEGER,
    cellVoltages TEXT,
    cellResist TEXT,
    chargeMOS INTEGER,
    dischargeMOS INTEGER,
    balancerOn INTEGER
);
CREATE TABLE IF NOT EXISTS gps_data (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    type TEXT NOT NULL,
    lon REAL,
    lat REAL,
    speed REAL,
    fix INTEGER
);
CREATE TABLE IF NOT EXISTS app_settings (
    key TEXT PRIMARY KEY,
    value TEXT
);
CREATE TABLE IF NOT EXISTS range_learning (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    startTs INTEGER,
    endTs INTEGER,
    distance REAL,
    capUsed REAL,
    learnedFactor REAL,
    oldFactor REAL,
    newFactor REAL
);
CREATE TABLE IF NOT EXISTS alarms (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    level TEXT NOT NULL,
    message TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS charge_cycles (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    startTime INTEGER NOT NULL,
    endTime INTEGER,
    startSOC INTEGER,
    endSOC INTEGER,
    startCap REAL,
    endCap REAL,
    chargedAh REAL
);
CREATE TABLE IF NOT EXISTS capacity_learning (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    startTs INTEGER,
    endTs INTEGER,
    capUsed REAL,
    oldCap REAL,
    newCap REAL
);
CREATE TABLE IF NOT EXISTS heartbeat (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    ts INTEGER NOT NULL,
    uid TEXT NOT NULL,
    page TEXT,
    ip TEXT
);
CREATE INDEX IF NOT EXISTS idx_bms_ts ON bms_data(ts);
CREATE INDEX IF NOT EXISTS idx_gps_ts ON gps_data(ts);
CREATE INDEX IF NOT EXISTS idx_gps_type ON gps_data(type);
`;

function open(dbPath) {
    if (!dbPath) dbPath = './data/bms.db';
    const dir = path.dirname(dbPath);
    if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
    db = new Database(dbPath);
    db.pragma('journal_mode = WAL');
    db.exec(SCHEMA);
    console.log(`[db] SQLite 已打开: ${dbPath}`);
    return db;
}

function get() { return db; }

/* ==================== app_settings ==================== */
function getSetting(key, def) {
    const row = db.prepare('SELECT value FROM app_settings WHERE key=?').get(key);
    return row ? row.value : (def === undefined ? null : def);
}
function setSetting(key, value) {
    db.prepare('INSERT INTO app_settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value')
        .run(key, String(value));
}
function getSettings() {
    const rows = db.prepare('SELECT key,value FROM app_settings').all();
    const o = {};
    for (const r of rows) o[r.key] = r.value;
    return o;
}

/* ==================== 最新缓存 (内存, 网页 API 秒读) ==================== */
const cache = {
    bms: null,          // 最新 BMS 行
    gps: null,          // 最新 GPS
    status: {},         // csq/blestatus 等
    lastBmsTs: 0,
    lastGpsTs: 0,
    lastAnyTs: 0,
    page: 'gps',
};

function getLatestBms() { return cache.bms; }
function getLatestGps() { return cache.gps; }
function getStatus() { return cache.status; }
function setPage(p) { cache.page = p; }
function getPage() { return cache.page; }

/* ==================== 数据保留 (对齐 PHP: dataRetentionDays) ==================== */
function cleanupOldData(days) {
    const cutoff = Math.floor(Date.now() / 1000) - days * 86400;
    for (const t of ['bms_data', 'gps_data']) {
        const n = db.prepare(`DELETE FROM ${t} WHERE ts < ?`).run(cutoff);
        if (n.changes > 0) console.log(`[db] 清理 ${t}: ${n.changes} 行 (保留 ${days} 天)`);
    }
    const hb = db.prepare('DELETE FROM heartbeat WHERE ts < ?').run(Math.floor(Date.now() / 1000) - 86400);
    if (hb.changes > 0) console.log(`[db] 清理 heartbeat: ${hb.changes} 行`);
}

module.exports = { open, get, getSetting, setSetting, getSettings, cache,
    getLatestBms, getLatestGps, getStatus, setPage, getPage, cleanupOldData };
