'use strict';
/* parser.js — BMS 原始帧解析 + 上行文本消息分发 (移植自 01-网页服务端/parser.php) */

function hexToBytes(hex) {
    const bytes = [];
    let i = 0;
    const len = hex.length;
    while (i < len) {
        if (hex[i] === '!' && i + 1 < len) {
            const end = hex.indexOf('!', i + 1);
            if (end !== -1 && end > i + 1) {
                const count = parseInt(hex.substring(i + 1, end), 10);
                if (count > 0) for (let j = 0; j < count; j++) bytes.push(0);
                i = end + 1;
                continue;
            }
        }
        if (i + 1 < len) {
            bytes.push(parseInt(hex.substring(i, i + 2), 16));
            i += 2;
        } else break;
    }
    return bytes;
}

function get16(b, i) {
    if (i + 1 >= b.length) return 0;
    return ((b[i + 1] << 8) | b[i]) & 0xFFFF;
}
function get32(b, i) {
    if (i + 3 >= b.length) return 0;
    return ((b[i] & 0xFF) | ((b[i + 1] & 0xFF) << 8) | ((b[i + 2] & 0xFF) << 16) | ((b[i + 3] & 0xFF) << 24)) >>> 0;
}
function getS16(b, i) {
    const v = get16(b, i);
    return v & 0x8000 ? v - 0x10000 : v;
}
function getS32(b, i) {
    const v = get32(b, i);
    return v & 0x80000000 ? v - 0x100000000 : v;
}
function countBits(n) {
    let c = 0;
    for (let i = 0; i < 32; i++) if (n & (1 << i)) c++;
    return c;
}

/** 0x01 设置帧：开关状态 + 额定容量 */
function parseJK01Settings(b) {
    const chargeMOS = (b[118] ?? 0) > 0;
    const dischargeMOS = (b[122] ?? 0) > 0;
    const balancerOn = (b[126] ?? 0) > 0;
    const capNominal = get32(b, 130) * 0.001;
    const capValid = capNominal >= 1 && capNominal <= 1000;
    return {
        frameType: 0x01,
        chargeMOS, dischargeMOS, balancerOn,
        capNominal: capValid ? Math.round(capNominal * 100) / 100 : null,
        capNominalValid: capValid,
    };
}

/** 0x02 电芯信息帧：24S/32S 自动识别 + 全字段解析 */
function parseJK02CellInfo(b) {
    const bm24 = get32(b, 54);
    const bm32 = get32(b, 70);
    const n24 = countBits(bm24);
    const n32 = countBits(bm32);
    const avg24 = get16(b, 58) * 0.001;
    const avg32 = get16(b, 74) * 0.001;
    const isAvg24Valid = avg24 >= 2.0 && avg24 <= 5.0;
    const isAvg32Valid = avg32 >= 2.0 && avg32 <= 5.0;

    let is32S, cellCount, totalCells, offset, off2;
    if (isAvg32Valid && !isAvg24Valid) { is32S = true; cellCount = n32; totalCells = 32; offset = 16; off2 = 32; }
    else if (isAvg24Valid && !isAvg32Valid) { is32S = false; cellCount = n24; totalCells = 24; offset = 0; off2 = 0; }
    else if (n32 > 0 && n24 === 0) { is32S = true; cellCount = n32; totalCells = 32; offset = 16; off2 = 32; }
    else if (n24 > 0 && n32 === 0) { is32S = false; cellCount = n24; totalCells = 24; offset = 0; off2 = 0; }
    else {
        let voltCells = 0;
        for (let i = 0; i < 32; i++) {
            const v = get16(b, 6 + i * 2) * 0.001;
            if (v >= 2.0 && v <= 5.0) voltCells++;
        }
        if (Math.abs(n32 - voltCells) <= Math.abs(n24 - voltCells)) { is32S = true; cellCount = n32; totalCells = 32; offset = 16; off2 = 32; }
        else { is32S = false; cellCount = n24; totalCells = 24; offset = 0; off2 = 0; }
    }
    if (cellCount === 0) cellCount = 16;

    const cellVoltages = [];
    for (let i = 0; i < totalCells; i++) {
        cellVoltages.push(Math.round(get16(b, 6 + i * 2) * 0.001 * 1000) / 1000);
    }
    const cellResist = [];
    for (let i = 0; i < totalCells; i++) {
        cellResist.push(Math.round(get16(b, 64 + offset + i * 2) * 0.001 * 1000) / 1000);
    }

    const totalVoltage = Math.round(get32(b, 118 + off2) * 0.001 * 1000) / 1000;
    const current = Math.round(getS32(b, 126 + off2) * 0.001 * 100) / 100;
    const power = Math.round(totalVoltage * current * 10) / 10;
    const temp1 = Math.round(getS16(b, 130 + off2) * 0.1 * 10) / 10;
    const temp2 = Math.round(getS16(b, 132 + off2) * 0.1 * 10) / 10;
    const mosTemp = Math.round(getS16(b, (is32S ? 112 : 134) + off2) * 0.1 * 10) / 10;
    const errors = get16(b, (is32S ? 134 : 136) + off2);
    const balanceCurr = Math.round(getS16(b, 138 + off2) * 0.001 * 1000) / 1000;
    const balancingAction = b[140 + off2] ?? 0;
    const soc = b[141 + off2] ?? 0;
    const capRemain = Math.round(get32(b, 142 + off2) * 0.001 * 100) / 100;
    const capNominal = Math.round(get32(b, 146 + off2) * 0.001 * 100) / 100;
    const cycleCount = get32(b, 150 + off2);
    const cycleCap = Math.round(get32(b, 154 + off2) * 0.001 * 1000) / 1000;
    const soh = b[158 + off2] ?? 0;
    const runtimeSec = get32(b, 162 + off2);
    const days = Math.floor(runtimeSec / 86400);
    const hrs = Math.floor((runtimeSec % 86400) / 3600);
    const mins = Math.floor((runtimeSec % 3600) / 60);
    const runtimeStr = `${days}d ${String(hrs).padStart(2, '0')}h ${String(mins).padStart(2, '0')}m`;

    const chargeMOS = (b[166 + off2] ?? 0) > 0;
    const dischargeMOS = (b[167 + off2] ?? 0) > 0;
    const balancerOn = (b[169 + off2] ?? 0) > 0;

    const validVoltages = cellVoltages.filter(v => v > 0);
    const maxCellV = validVoltages.length ? Math.max(...validVoltages) : null;
    const minCellV = validVoltages.length ? Math.min(...validVoltages) : null;
    const avgCellV = validVoltages.length ? Math.round(validVoltages.reduce((a, b) => a + b, 0) / validVoltages.length * 1000) / 1000 : null;
    const deltaCellV = (maxCellV !== null && minCellV !== null) ? Math.round((maxCellV - minCellV) * 10000) / 10000 : null;

    return {
        frameType: 0x02, cellCount, totalCells, cellVoltages, cellResist,
        totalVoltage, current, power, temp1, temp2, mosTemp, errors, balanceCurr,
        balancingAction, soc, capRemain, capNominal, cycleCount, cycleCap, soh,
        runtimeStr, runtimeSec, chargeMOS, dischargeMOS, balancerOn,
        maxCellV, minCellV, avgCellV, deltaCellV,
        frameCounter: b[5] ?? 0, is32S,
    };
}

/** 解析 BMS 原始帧入口（校验帧头 + CRC，按帧类型分发） */
function parseBMSRaw(hex) {
    const b = hexToBytes(hex);
    if (b.length < 130) return null;
    if (b[0] !== 0x55 || b[1] !== 0xAA || b[2] !== 0xEB || b[3] !== 0x90) return null;
    let crc = 0;
    for (let i = 0; i < b.length - 1; i++) crc = (crc + b[i]) & 0xFF;
    const crcOk = crc === b[b.length - 1];
    if (!crcOk && b.length >= 290) return null;
    const ftype = b[4];
    if (ftype === 0x02) return parseJK02CellInfo(b);
    if (ftype === 0x01) return parseJK01Settings(b);
    return null;
}

/** 文本消息统一分发 (对齐 parser.php parseTextMessage) */
function parseTextMessage(txt) {
    txt = String(txt || '').trim();
    if (!txt) return { type: null, data: null };

    // ── GPS ──
    if (txt.startsWith('gps:')) {
        const p = txt.substring(4).split(',');
        if (p.length >= 6) {
            if (p[0].trim() !== '1') return { type: 'gps', data: { fix: false } };
            const lon = parseFloat(p[2]);
            const lat = parseFloat(p[4]);
            const speed = parseFloat(p[5] ?? 0);
            if (lon === 0 || lat === 0) return { type: 'gps', data: { fix: false } };
            const data = { fix: true, lon, lat, speed };
            if (p[6] !== undefined && p[6] !== '') data.sat = parseInt(p[6], 10);
            if (p[7] !== undefined && p[7] !== '') data.useSat = parseInt(p[7], 10);
            if (p[8] !== undefined && p[8] !== '') data.hdop = parseFloat(p[8]);
            return { type: 'gps', data };
        }
        if (p.length >= 2) {
            const lon = parseFloat(p[0]);
            const lat = parseFloat(p[1]);
            const speed = parseFloat(p[2] ?? 0);
            if (lon <= 0 || lat <= 0 || lon >= 180 || lat >= 90) return { type: 'gps', data: { fix: false } };
            return { type: 'gps', data: { fix: true, lon, lat, speed } };
        }
        return { type: 'gps', data: { fix: false } };
    }

    // ── LBS ──
    if (txt.startsWith('lbs:')) {
        const p = txt.substring(4).split(',');
        if (p.length >= 2) {
            const lon = parseFloat(p[0]);
            const lat = parseFloat(p[1]);
            if (lon > 0 && lat > 0) return { type: 'jzdw', data: { lon, lat } };
        }
        return { type: null, data: null };
    }
    const m = txt.match(/^(\d+\.\d+)_(\d+\.\d+)$/);
    if (m) {
        const lon = parseFloat(m[1]);
        const lat = parseFloat(m[2]);
        if (lon > 0 && lat > 0 && lon < 180 && lat < 90) return { type: 'jzdw', data: { lon, lat } };
    }

    // ── BMS 原始帧 ──
    if (txt.startsWith('bms:raw,')) return { type: 'bms', data: parseBMSRaw(txt.substring(8)) };
    if (txt.startsWith('b:r,')) return { type: 'bms', data: parseBMSRaw(txt.substring(4)) };

    // ── 蓝牙状态 ──
    if (txt.startsWith('blestatus:')) {
        const p = txt.substring(10).split(',');
        return { type: 'blestatus', data: { state: p[0] ?? 'unknown', mac: p[1] ?? '', connected: (p[2] ?? '0') === '1' } };
    }

    // ── 4G 信号 ──
    if (txt.startsWith('csq:')) return { type: 'csq', data: { csq: parseInt(txt.substring(4), 10) } };

    // ── 中继状态 ──
    if (txt.startsWith('relay:on')) return { type: 'relay', data: { enabled: true } };
    if (txt.startsWith('relay:off')) return { type: 'relay', data: { enabled: false } };
    if (txt.startsWith('relaystatus:')) {
        const p = txt.substring(12).split(',');
        return { type: 'relay', data: { enabled: (p[0] ?? '') === 'on' } };
    }

    // ── 开关执行结果 ──
    if (txt.startsWith('switch:')) {
        const parts = txt.substring(7).split(':');
        if (parts.length >= 2) {
            const keyMap = { charge: 'chargeMOS', discharge: 'dischargeMOS', balance: 'balancerOn' };
            if (keyMap[parts[0]]) {
                return { type: 'switch', data: { key: keyMap[parts[0]], enabled: parts[1] === 'on', error: parts[1] === 'error' } };
            }
        }
    }

    // ── 上报频率查询结果 ──
    if (txt.startsWith('sendfreq:')) {
        const result = {};
        for (const pair of txt.substring(9).split(',')) {
            const kv = pair.split('=');
            if (kv.length === 2) result[kv[0]] = parseInt(kv[1], 10);
        }
        return { type: 'sendfreq', data: result };
    }

    // ── 自动均衡状态 ──
    if (txt.startsWith('autobal:')) {
        const body = txt.substring(8);
        if (body.includes('=')) {
            const result = {};
            for (const pair of body.split(',')) {
                const kv = pair.split('=');
                if (kv.length === 2) {
                    const k = kv[0].trim();
                    result[k] = k.endsWith('_volt') ? parseFloat(kv[1]) : kv[1].trim() === 'on';
                }
            }
            return { type: 'autobal', data: result };
        }
        const parts = body.split(',');
        if (parts.length >= 2) {
            const result = {};
            if (parts[0] === 'charge' || parts[0] === 'static') {
                result[parts[0]] = parts[1] === 'on';
                if (parts[2] !== undefined) result[parts[0] + '_volt'] = parseFloat(parts[2]);
                return { type: 'autobal', data: result };
            }
            if (parts[0] === 'volt' && parts.length >= 3) {
                result[parts[1] + '_volt'] = parseFloat(parts[2]);
                return { type: 'autobal', data: result };
            }
        }
        return { type: 'autobal', data: {} };
    }

    return { type: null, data: null };
}

module.exports = { hexToBytes, get16, get32, getS16, getS32, countBits, parseBMSRaw, parseJK01Settings, parseJK02CellInfo, parseTextMessage };
