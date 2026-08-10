'use strict';
/* insights.js — 告警 / 充电循环 / 容量校准 / 里程学习 / 里程计算 (移植自 insight.php + range.php)
   所有状态持久化到 app_settings (Node 常驻进程跨请求本可用内存, 但保留 SQLite 以便重启不丢状态) */
const db = require('./db');

/* ==================== 工具: 坐标 ==================== */
function haversine(lat1, lon1, lat2, lon2) {
    const R = 6371.0;
    const dLat = (lat2 - lat1) * Math.PI / 180;
    const dLon = (lon2 - lon1) * Math.PI / 180;
    const a = Math.sin(dLat / 2) ** 2 +
        Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) * Math.sin(dLon / 2) ** 2;
    return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function wgs84ToGcj02(wgsLat, wgsLng) {
    const a = 6378245.0;
    const ee = 0.00669342162296594323;
    const transformLat = (x, y) => {
        let ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * Math.sqrt(Math.abs(x));
        ret += (20.0 * Math.sin(6.0 * x * Math.PI) + 20.0 * Math.sin(2.0 * x * Math.PI)) * 2.0 / 3.0;
        ret += (20.0 * Math.sin(y * Math.PI) + 40.0 * Math.sin(y / 3.0 * Math.PI)) * 2.0 / 3.0;
        ret += (160.0 * Math.sin(y / 12.0 * Math.PI) + 320.0 * Math.sin(y * Math.PI / 30.0)) * 2.0 / 3.0;
        return ret;
    };
    const transformLng = (x, y) => {
        let ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * Math.sqrt(Math.abs(x));
        ret += (20.0 * Math.sin(6.0 * x * Math.PI) + 20.0 * Math.sin(2.0 * x * Math.PI)) * 2.0 / 3.0;
        ret += (20.0 * Math.sin(x * Math.PI) + 40.0 * Math.sin(x / 3.0 * Math.PI)) * 2.0 / 3.0;
        ret += (150.0 * Math.sin(x / 12.0 * Math.PI) + 300.0 * Math.sin(x / 30.0 * Math.PI)) * 2.0 / 3.0;
        return ret;
    };
    if (wgsLng < 72.004 || wgsLng > 137.8347 || wgsLat < 0.8293 || wgsLat > 55.8271) {
        return [wgsLat, wgsLng];
    }
    const dLat = transformLat(wgsLng - 105.0, wgsLat - 35.0);
    const dLng = transformLng(wgsLng - 105.0, wgsLat - 35.0);
    const radLat = wgsLat / 180.0 * Math.PI;
    let magic = Math.sin(radLat);
    magic = 1 - ee * magic * magic;
    const sqrtMagic = Math.sqrt(magic);
    const nLat = (dLat * 180.0) / ((a * (1 - ee)) / (magic * sqrtMagic) * Math.PI);
    const nLng = (dLng * 180.0) / (a / sqrtMagic * Math.cos(radLat) * Math.PI);
    return [Math.round((wgsLat + nLat) * 1e6) / 1e6, Math.round((wgsLng + nLng) * 1e6) / 1e6];
}

/* ==================== 里程 ==================== */
function getRangeFactor(cfg) {
    const v = db.getSetting('rangeFactor');
    return v !== null ? parseFloat(v) : (cfg.settings.rangeFactorDefault ?? 2.0);
}
function calcRangeKm(capRemain, cfg) {
    if (capRemain === null || capRemain <= 0) return null;
    return Math.round(capRemain * getRangeFactor(cfg) * 10) / 10;
}

function calculateLearnedFactor(startTs, endTs, capUsed, cfg, log) {
    const rows = db.get().prepare('SELECT lat, lon FROM gps_data WHERE ts >= ? AND ts <= ? AND fix = 1 ORDER BY ts ASC')
        .all(startTs, endTs);
    if (rows.length < 2) { log(`[里程学习] GPS 数据不足(${rows.length}点), 跳过本次计算`); return; }
    let distance = 0;
    for (let i = 1; i < rows.length; i++) {
        distance += haversine(rows[i - 1].lat, rows[i - 1].lon, rows[i].lat, rows[i].lon);
    }
    if (distance < 0.2) { log(`[里程学习] 距离过短(${distance.toFixed(2)}km), 跳过本次计算`); return; }
    const learnedFactor = Math.round(distance / capUsed * 100) / 100;
    const oldFactor = getRangeFactor(cfg);
    const newFactor = Math.round((oldFactor * 0.7 + learnedFactor * 0.3) * 100) / 100;
    db.setSetting('rangeFactor', String(newFactor));
    db.get().prepare('INSERT INTO range_learning (ts, startTs, endTs, distance, capUsed, learnedFactor, oldFactor, newFactor) VALUES (?,?,?,?,?,?,?,?)')
        .run(Date.now(), startTs, endTs, Math.round(distance * 100) / 100, Math.round(capUsed * 100) / 100, learnedFactor, oldFactor, newFactor);
    log(`[里程学习] 完成: 距离=${distance.toFixed(2)}km 消耗=${capUsed.toFixed(2)}Ah 学习基数=${learnedFactor} 新基数=${newFactor}(旧=${oldFactor})`);
}

/** 随充随学 (对齐 range.php updateRangeLearning) */
function updateRangeLearning(data, cfg, log) {
    if (db.getSetting('learningEnabled', 'true') !== 'true') {
        db.setSetting('learnWasCharging', 'false');
        return;
    }
    const current = data.current ?? 0;
    const isCharging = current > 0.1;
    const capRemain = parseFloat(data.capRemain ?? 0);
    const now = Date.now();
    const wasCharging = db.getSetting('learnWasCharging', 'false') === 'true';
    const lastEndCap = parseFloat(db.getSetting('learnLastEndCap', '0'));
    const lastEndTs = parseInt(db.getSetting('learnEndTs', '0'), 10);

    if (isCharging && !wasCharging) {
        if (lastEndCap > 0 && lastEndTs > 0) {
            const capUsed = lastEndCap - capRemain;
            if (capUsed > 0.5) calculateLearnedFactor(lastEndTs, now, capUsed, cfg, log);
            else if (capUsed > 0) log(`[里程学习] 消耗过少(${capUsed.toFixed(2)}Ah), 跳过本次计算`);
        }
        db.setSetting('learnLastEndCap', '0');
        db.setSetting('learnEndTs', '0');
    }
    if (!isCharging && wasCharging) {
        db.setSetting('learnLastEndCap', String(capRemain));
        db.setSetting('learnEndTs', String(now));
        log(`[里程学习] 充电结束, 记录基准容量=${capRemain.toFixed(2)}Ah (下次充电开始时结算此周期 km/Ah)`);
    }
    db.setSetting('learnWasCharging', isCharging ? 'true' : 'false');
}

/* ==================== 告警 ==================== */
function checkAlarms(data, cfg, log) {
    const a = cfg.settings.alarmThresholds ?? {};
    if (db.getSetting('alarmsEnabled', 'true') !== 'true') return;
    for (const [idx, v] of (data.cellVoltages ?? []).entries()) {
        if (v >= (a.maxCellV ?? 3.65)) addAlarm('critical', `电芯 C${String(idx + 1).padStart(2, '0')} 过压: ${v.toFixed(3)}V`, log);
        else if (v > 0 && v <= (a.minCellV ?? 2.80)) addAlarm('critical', `电芯 C${String(idx + 1).padStart(2, '0')} 欠压: ${v.toFixed(3)}V`, log);
    }
    const delta = parseFloat(data.deltaCellV ?? 0);
    if (delta >= (a.deltaCellV ?? 0.05)) addAlarm('warning', `单体压差过大: ${Math.round(delta * 1000)}mV`, log);
    const maxT = Math.max(parseFloat(data.temp1 ?? 0), parseFloat(data.temp2 ?? 0), parseFloat(data.mosTemp ?? 0));
    if (maxT >= (a.maxTemp ?? 60)) addAlarm('critical', `温度过高: ${maxT.toFixed(1)}℃`, log);
    const soc = parseInt(data.soc ?? 100, 10);
    if (soc <= (a.minSOC ?? 20)) addAlarm('warning', `SOC过低: ${soc}%`, log);
}

function addAlarm(level, message, log) {
    const exists = db.get().prepare('SELECT id FROM alarms WHERE ts > ? AND message = ?').get(Date.now() - 300000, message);
    if (exists) return;
    db.get().prepare('INSERT INTO alarms (ts, level, message) VALUES (?,?,?)').run(Date.now(), level, message);
    log(`[告警][${level}] ${message}`);
}

/* ==================== 充电循环 ==================== */
function checkChargeCycle(data, cfg, log) {
    if (db.getSetting('chargeCyclesEnabled', 'true') !== 'true') return;
    const current = parseFloat(data.current ?? 0);
    const isCharging = current > 0.1;
    const soc = parseInt(data.soc ?? 0, 10);
    const cap = parseFloat(data.capRemain ?? 0);
    const now = Date.now();
    const active = db.getSetting('chgActive', 'false') === 'true';
    const startTs = parseInt(db.getSetting('chgStartTs', '0'), 10);
    const startSoc = parseInt(db.getSetting('chgStartSoc', '0'), 10);
    const startCap = parseFloat(db.getSetting('chgStartCap', '0'));

    if (isCharging && !active) {
        db.setSetting('chgActive', 'true');
        db.setSetting('chgStartTs', String(now));
        db.setSetting('chgStartSoc', String(soc));
        db.setSetting('chgStartCap', String(cap));
    } else if (!isCharging && active) {
        const chargedAh = Math.round((cap - startCap) * 100) / 100;
        db.get().prepare('INSERT INTO charge_cycles (startTime, endTime, startSOC, endSOC, startCap, endCap, chargedAh) VALUES (?,?,?,?,?,?,?)')
            .run(startTs, now, startSoc, soc, startCap, cap, chargedAh);
        log(`[充电循环] 完成: ${startCap.toFixed(2)}→${cap.toFixed(2)}Ah (充入 ${chargedAh.toFixed(2)}Ah, SOC ${startSoc}→${soc}%)`);
        db.setSetting('chgActive', 'false');
        db.setSetting('chgStartTs', '0');
        db.setSetting('chgStartSoc', '0');
        db.setSetting('chgStartCap', '0');
    }
}

/* ==================== 容量校准 ==================== */
function updateCapacityLearning(data, cfg, log) {
    const capCfg = cfg.settings.capLearningEnabled ?? true;
    if (!capCfg) return;
    if (db.getSetting('capLearningEnabled', 'true') !== 'true') return;
    const current = parseFloat(data.current ?? 0);
    const isCharging = current > 0.1;
    const isDischarge = current < -0.1;
    const soc = parseInt(data.soc ?? 0, 10);
    const capRemain = parseFloat(data.capRemain ?? 0);
    const now = Date.now();
    const phase = db.getSetting('capPhase', 'idle');
    const startTs = parseInt(db.getSetting('capStartTs', '0'), 10);
    const startCap = parseFloat(db.getSetting('capStartCap', '0'));
    const wasDischg = db.getSetting('capWasDischarging', 'false') === 'true';

    if (phase === 'idle') {
        if (soc >= 99 && isCharging) {
            db.setSetting('capPhase', 'tracking');
            db.setSetting('capStartTs', String(now));
            db.setSetting('capStartCap', String(capRemain));
            db.setSetting('capWasDischarging', 'false');
            log(`[容量校准] 检测到满充, 开始追踪放电周期 (SOC=${soc}%, cap=${capRemain.toFixed(2)}Ah)`);
        }
    } else {
        if (isCharging && wasDischg) {
            const capUsed = startCap - capRemain;
            const minAh = cfg.settings.capLearningMinAh ?? 5;
            if (capUsed >= minAh) {
                let oldCap = parseFloat(db.getSetting('calibratedCapacity', startCap > 0 ? String(startCap) : '0'));
                if (oldCap <= 0) oldCap = startCap;
                const newCap = Math.round((oldCap * 0.7 + capUsed * 0.3) * 100) / 100;
                db.setSetting('calibratedCapacity', String(newCap));
                db.get().prepare('INSERT INTO capacity_learning (ts, startTs, endTs, capUsed, oldCap, newCap) VALUES (?,?,?,?,?,?)')
                    .run(now, startTs, now, Math.round(capUsed * 100) / 100, oldCap, newCap);
                log(`[容量校准] 完成: 消耗=${capUsed.toFixed(2)}Ah, 校准容量 ${oldCap.toFixed(2)}→${newCap.toFixed(2)}Ah`);
            } else {
                log(`[容量校准] 消耗过少(${capUsed.toFixed(2)}Ah<${minAh}Ah), 跳过`);
            }
            db.setSetting('capPhase', 'idle');
            db.setSetting('capWasDischarging', 'false');
        }
        if (isDischarge) db.setSetting('capWasDischarging', 'true');
    }
}

module.exports = { haversine, wgs84ToGcj02, getRangeFactor, calcRangeKm, updateRangeLearning, calculateLearnedFactor, checkAlarms, addAlarm, checkChargeCycle, updateCapacityLearning };
