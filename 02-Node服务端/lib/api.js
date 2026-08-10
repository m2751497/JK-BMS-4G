'use strict';
/* api.js — REST API + 鉴权 (对齐 PHP api.php 的 ?action= 返回结构; 另提供 REST 风格给浅色网页) */
const crypto = require('crypto');
const { parse } = require('url');

/* ==================== 鉴权 (scrypt + 内存 session + IP 限速) ==================== */
function createAuth(config, db) {
    const sessions = new Map();      // token -> expiry(ms)
    const fails = new Map();         // ip -> {fails, firstTs}
    const a = config.auth;

    function hashPassword(pw, salt = crypto.randomBytes(16).toString('hex')) {
        const hash = crypto.scryptSync(String(pw), salt, 64).toString('hex');
        return `scrypt$${salt}$${hash}`;
    }
    function verifyPassword(pw, stored) {
        if (!stored || !stored.startsWith('scrypt$')) {
            // 兼容明文 (config.example 模板 / 首次)
            return stored === String(pw);
        }
        const [_, salt, hash] = stored.split('$');
        const calc = crypto.scryptSync(String(pw), salt, 64).toString('hex');
        return crypto.timingSafeEqual(Buffer.from(hash, 'hex'), Buffer.from(calc, 'hex'));
    }

    function isLocked(ip) {
        const f = fails.get(ip);
        if (!f) return false;
        if (f.fails >= (a.maxFails ?? 5)) {
            if (Date.now() - f.firstTs < (a.lockSeconds ?? 300) * 1000) return true;
            fails.delete(ip);
        }
        return false;
    }
    function recordFailure(ip) {
        const f = fails.get(ip) || { fails: 0, firstTs: 0 };
        if (f.fails === 0) f.firstTs = Date.now();
        f.fails++;
        fails.set(ip, f);
    }

    function login(user, pass, ip) {
        if (isLocked(ip)) return { ok: false, msg: '失败次数过多, 已锁定, 请稍后再试' };
        if (user === a.username && verifyPassword(pass, a.password)) {
            fails.delete(ip);
            const token = crypto.randomBytes(32).toString('hex');
            sessions.set(token, Date.now() + (a.sessionDays ?? 7) * 86400 * 1000);
            return { ok: true, token };
        }
        recordFailure(ip);
        return { ok: false, msg: '账号或密码错误' };
    }
    function check(token) {
        if (!a.enabled) return true;
        if (!token) return false;
        const exp = sessions.get(token);
        if (!exp) return false;
        if (Date.now() > exp) { sessions.delete(token); return false; }
        return true;
    }
    return { login, check, sessions };
}

/* ==================== 公共查询 ==================== */
function trackQuery(db, insights, from, to) {
    const rows = db.get().prepare('SELECT ts, lon, lat, speed FROM gps_data WHERE ts >= ? AND ts <= ? AND fix = 1 ORDER BY ts ASC').all(from, to);
    const points = [];
    let distance = 0, prev = null, firstTs = null, lastTs = null;
    const speeds = [];
    for (const r of rows) {
        const lat = parseFloat(r.lat), lon = parseFloat(r.lon), spd = parseFloat(r.speed);
        if (firstTs === null) firstTs = r.ts;
        lastTs = r.ts;
        speeds.push(spd);
        if (prev) distance += insights.haversine(prev[0], prev[1], lat, lon);
        prev = [lat, lon];
        const [gcjLat, gcjLng] = insights.wgs84ToGcj02(lat, lon);
        points.push({ ts: r.ts, lon, lat, speed: spd, gcjLng, gcjLat });
    }
    const count = speeds.length;
    return {
        count,
        distance: Math.round(distance * 100) / 100,
        avgSpeed: count ? Math.round(speeds.reduce((a, b) => a + b, 0) / count * 10) / 10 : 0,
        maxSpeed: count ? Math.round(Math.max(...speeds) * 10) / 10 : 0,
        durationSec: (firstTs !== null && lastTs !== null) ? Math.max(0, Math.floor((lastTs - firstTs) / 1000)) : 0,
        points,
    };
}

function tripsQuery(db, insights, date) {
    const rows = db.get().prepare("SELECT ts, lon, lat, speed FROM gps_data WHERE date(ts / 1000, 'unixepoch', 'localtime') = ? AND fix = 1 ORDER BY ts ASC").all(date);
    const SPLIT_GAP = 300, MOVE_DIST = 0.020;
    const trips = [];
    let cur = null, prevTs = null, prevLat = null, prevLon = null;
    for (const r of rows) {
        const ts = Math.floor(r.ts / 1000), lat = parseFloat(r.lat), lon = parseFloat(r.lon), spd = parseFloat(r.speed);
        if (cur === null || (prevTs !== null && ts - prevTs > SPLIT_GAP)) {
            if (cur) trips.push(cur);
            cur = { startTs: ts, endTs: ts, dist: 0, dur: 0, spds: [spd], count: 1 };
            prevTs = ts; prevLat = lat; prevLon = lon;
            continue;
        }
        cur.endTs = ts; cur.spds.push(spd); cur.count++;
        const d = insights.haversine(prevLat, prevLon, lat, lon);
        cur.dist += d;
        if (d > MOVE_DIST) cur.dur += (ts - prevTs);
        prevTs = ts; prevLat = lat; prevLon = lon;
    }
    if (cur) trips.push(cur);
    const out = trips.filter(t => t.count >= 2).map(t => {
        const distKm = Math.round(t.dist * 100) / 100;
        return {
            startTs: t.startTs, endTs: t.endTs, distanceKm: distKm,
            durationSec: t.dur,
            avgSpeed: t.dur > 0 ? Math.round(distKm / (t.dur / 3600) * 10) / 10 : 0,
            maxSpeed: Math.round(Math.max(...t.spds) * 10) / 10,
            count: t.count,
        };
    });
    return { date, trips: out };
}

function replayQuery(db, insights, date, from, to) {
    let rows, d = date || '';
    if (from !== undefined || to !== undefined) {
        rows = db.get().prepare('SELECT ts, lon, lat, speed FROM gps_data WHERE ts >= ? AND ts <= ? AND fix = 1 ORDER BY ts ASC')
            .all(from ?? 0, to ?? Date.now());
    } else {
        rows = db.get().prepare("SELECT ts, lon, lat, speed FROM gps_data WHERE date(ts / 1000, 'unixepoch', 'localtime') = ? ORDER BY ts ASC").all(d);
    }
    const points = rows.map(r => {
        const lat = parseFloat(r.lat), lon = parseFloat(r.lon);
        const [gcjLat, gcjLng] = insights.wgs84ToGcj02(lat, lon);
        return { t: Math.floor(r.ts / 1000), lat, lon, speed: parseFloat(r.speed), gcjLat, gcjLng };
    });
    return { date: d, count: points.length, points };
}

/* ==================== 路由注册 ==================== */
module.exports = function register(app, ctx) {
    const { config, db, insights, mqttClient, ingestLog, online, onlineNow, refreshMode } = ctx;
    const auth = createAuth(config, db);

    const S = config.settings;

    function send(res, code, obj) {
        res.status(code).json(obj);
    }
    function getBody(req) { return req.body || {}; }
    function ipOf(req) { return req.headers['x-forwarded-for']?.split(',')[0]?.trim() || req.socket.remoteAddress || 'unknown'; }
    function tokenOf(req) {
        const h = req.headers['x-auth-token'];
        if (h) return h;
        const ck = (req.headers.cookie || '').split(';').map(x => x.trim());
        for (const c of ck) if (c.startsWith('bms_token=')) return c.slice(10);
        return null;
    }

    // 鉴权中间件 (auth 关闭时全放行)
    function requireAuth(req, res, next) {
        if (!config.auth.enabled) return next();
        if (!auth.check(tokenOf(req))) return send(res, 401, { error: '未登录或已过期' });
        next();
    }
    function setAuthCookie(res, token) {
        res.setHeader('Set-Cookie', `bms_token=${token}; Path=/; Max-Age=${(config.auth.sessionDays ?? 7) * 86400}; HttpOnly; SameSite=Lax`);
    }

    /* ── 登录 / 登出 ── */
    app.post('/api/login', (req, res) => {
        const { username, password } = getBody(req);
        const r = auth.login(username || '', password || '', ipOf(req));
        if (!r.ok) return send(res, 401, { error: r.msg });
        setAuthCookie(res, r.token);
        send(res, 200, { ok: true });
    });
    app.post('/api/logout', (req, res) => {
        const t = tokenOf(req);
        if (t) auth.sessions.delete(t);
        send(res, 200, { ok: true });
    });
    app.get('/api/authcheck', (req, res) => {
        send(res, 200, { ok: config.auth.enabled ? auth.check(tokenOf(req)) : true });
    });

    /* ── HTTP ingest 兼容 (旧设备/测试直报) ── */
    app.post('/api/ingest', (req, res) => {
        const token = req.headers['x-ingest-token'];
        const expect = config.mqtt.ingestToken || config.mqtt.password;
        if (!expect || token !== expect) return send(res, 403, { error: 'forbidden' });
        // express.json 只解析 application/json; ingest 上行是纯文本, 手动读流
        let txt = '';
        req.on('data', (c) => { txt += c; });
        req.on('end', () => {
            const body = txt.trim();
            if (ctx.handleUplink && body) ctx.handleUplink(body);
            send(res, 200, { ok: true });
        });
        req.on('error', (e) => send(res, 400, { error: e.message }));
    });

    /* ── 统一 action 分发 (兼容 PHP 前端) ── */
    app.all('/api', requireAuth, (req, res) => {
        const action = req.query.action || '';
        const method = req.method.toUpperCase();
        try {
            handleAction(action, method, req, res);
        } catch (e) {
            ingestLog(`[api] ${action} 异常: ${e.message}`);
            send(res, 500, { error: '服务器内部错误' });
        }
    });

    /* ── REST 风格 (浅色网页用) ── */
    app.get('/api/latest', requireAuth, (req, res) => handleAction('latest', 'GET', req, res));
    app.get('/api/status', requireAuth, (req, res) => handleAction('latest', 'GET', req, res));
    app.get('/api/track', requireAuth, (req, res) => handleAction('track', 'GET', req, res));
    app.get('/api/track/dates', requireAuth, (req, res) => handleAction('trackdates', 'GET', req, res));
    app.get('/api/trips', requireAuth, (req, res) => handleAction('trips', 'GET', req, res));
    app.get('/api/replay', requireAuth, (req, res) => handleAction('replay', 'GET', req, res));
    app.get('/api/range', requireAuth, (req, res) => handleAction('range', 'GET', req, res));
    app.post('/api/range', requireAuth, (req, res) => handleAction('range', 'POST', req, res));
    app.get('/api/settings', requireAuth, (req, res) => handleAction('settings', 'GET', req, res));
    app.post('/api/settings', requireAuth, (req, res) => handleAction('settings', 'POST', req, res));
    app.get('/api/alarms', requireAuth, (req, res) => handleAction('alarms', 'GET', req, res));
    app.get('/api/chargecycles', requireAuth, (req, res) => handleAction('chargecycles', 'GET', req, res));
    app.get('/api/capacity', requireAuth, (req, res) => handleAction('capacity', 'GET', req, res));
    app.post('/api/capacity', requireAuth, (req, res) => handleAction('capacity', 'POST', req, res));
    app.post('/api/mode', requireAuth, (req, res) => {
        const mode = getBody(req).mode === 'realtime' ? 'realtime' : 'track';
        const ok = refreshMode();
        send(res, 200, { ok: true, mode, online: ok.online, page: ok.page });
    });
    app.post('/api/reboot', requireAuth, (req, res) => {
        // Node 版重启 ESP32: 通过 MQTT 下行指令 (需固件支持) 或配置的 HTTP 地址
        if (config.espHttp) {
            const http = require('http');
            const u = new URL(config.espHttp + '/api/reboot');
            const rq = http.request(u, { method: 'POST' }, (r) => send(res, 200, { ok: r.statusCode < 400 }));
            rq.on('error', (e) => send(res, 500, { error: e.message }));
            rq.end();
        } else {
            send(res, 200, { ok: true, note: 'Node 版未配置 espHttp, 已忽略(设备重启需固件侧支持)' });
        }
    });

    /* ==================== action 实现 ==================== */
    function handleAction(action, method, req, res) {
        const now = Date.now();
        switch (action) {
            case 'latest': {
                const cache = db.cache;
                const rangeFactor = insights.getRangeFactor(config);
                send(res, 200, {
                    ts: cache.lastAnyTs || null,
                    lastUpdate: cache.lastAnyTs ? new Date(cache.lastAnyTs).toLocaleString('zh-CN', { hour12: false }) : null,
                    online: onlineNow(),
                    mode: db.getPage() === 'bms' && onlineNow() > 0 ? 'realtime' : 'track',
                    bms: cache.bms,
                    gps: cache.gps,
                    status: cache.status,
                    range: { rangeFactor, rangeKm: insights.calcRangeKm(cache.bms?.capRemain ?? null, config) },
                    settings: db.getSettings(),
                });
                break;
            }
            case 'heartbeat': {
                const uid = String(req.query.uid || 'web_' + ipOf(req)).slice(0, 64);
                const page = String(req.query.page || 'gps');
                const ip = ipOf(req);
                db.get().prepare('INSERT INTO heartbeat (ts, uid, page, ip) VALUES (?,?,?,?)').run(now, uid, page, ip);
                db.get().prepare('DELETE FROM heartbeat WHERE id NOT IN (SELECT id FROM heartbeat ORDER BY id DESC LIMIT 50)').run();
                // 维护在线表 (网页心跳 = 在线)
                online.set(uid, { lastSeen: Date.now() / 1000, page });
                db.setPage(page);
                refreshMode();
                send(res, 200, { ok: true, ts: now });
                break;
            }
            case 'track': {
                const from = req.query.from !== undefined ? parseInt(req.query.from, 10) : null;
                const to = req.query.to !== undefined ? parseInt(req.query.to, 10) : now;
                const start = from ?? (() => {
                    const d = new Date(); d.setHours(0, 0, 0, 0); return d.getTime();
                })();
                send(res, 200, trackQuery(db, insights, start, to));
                break;
            }
            case 'trackdates': {
                const rows = db.get().prepare("SELECT DISTINCT date(ts / 1000, 'unixepoch', 'localtime') AS d FROM gps_data ORDER BY d DESC").all();
                send(res, 200, { dates: rows.map(r => r.d) });
                break;
            }
            case 'trips': {
                const date = String(req.query.date || new Date().toISOString().slice(0, 10)).replace(/[^0-9\-]/g, '');
                send(res, 200, tripsQuery(db, insights, date));
                break;
            }
            case 'replay': {
                const date = String(req.query.date || '').replace(/[^0-9\-]/g, '');
                const from = req.query.from !== undefined ? parseInt(req.query.from, 10) : undefined;
                const to = req.query.to !== undefined ? parseInt(req.query.to, 10) : undefined;
                send(res, 200, replayQuery(db, insights, date, from, to));
                break;
            }
            case 'range': {
                if (method === 'POST') {
                    const body = getBody(req);
                    if (body.rangeFactor !== undefined) {
                        const f = Math.max(0.1, Math.min(100, parseFloat(body.rangeFactor) || S.rangeFactorDefault));
                        db.setSetting('rangeFactor', String(f));
                        ingestLog(`[设置] 里程基数手动改为 ${f} km/Ah`);
                    }
                    if (body.learningEnabled !== undefined) {
                        db.setSetting('learningEnabled', body.learningEnabled ? 'true' : 'false');
                    }
                    if (body.amapKey !== undefined) db.setSetting('amapKey', String(body.amapKey));
                    send(res, 200, { ok: true });
                } else {
                    const rows = db.get().prepare('SELECT * FROM range_learning ORDER BY id DESC LIMIT 20').all();
                    send(res, 200, {
                        rangeFactor: insights.getRangeFactor(config),
                        learningEnabled: db.getSetting('learningEnabled', 'true') === 'true',
                        learnPhase: db.getSetting('learnWasCharging', 'false') === 'true' ? 'charging' : 'idle',
                        learnHistory: rows.map(r => ({ ts: r.ts, distance: r.distance, capUsed: r.capUsed, learnedFactor: r.learnedFactor, oldFactor: r.oldFactor, newFactor: r.newFactor })),
                    });
                }
                break;
            }
            case 'settings': {
                if (method === 'POST') {
                    const body = getBody(req);
                    const numeric = ['onlineTimeout', 'realtimeBmsSec', 'realtimeGpsSec', 'trackGpsSec', 'dataRetentionDays', 'amapKey', 'authPassword'];
                    for (const [k, v] of Object.entries(body)) {
                        if (k === 'authPassword' && v) {
                            // 密码在线修改: 明文暂存, 提示重启生效 (简化; 生产可改 scrypt)
                            db.setSetting('authPassword', String(v));
                            continue;
                        }
                        if (typeof v === 'boolean') { db.setSetting(k, v ? 'true' : 'false'); continue; }
                        if (numeric.includes(k)) { db.setSetting(k, String(parseFloat(v))); continue; }
                        db.setSetting(k, String(v));
                    }
                    ingestLog(`[设置] 更新: ${Object.keys(body).join(', ')}`);
                    send(res, 200, { ok: true });
                } else {
                    send(res, 200, db.getSettings());
                }
                break;
            }
            case 'alarms': {
                const hours = parseInt(req.query.hours, 10) || 24;
                const rows = db.get().prepare('SELECT * FROM alarms WHERE ts >= ? ORDER BY id DESC LIMIT 200').all(now - hours * 3600 * 1000);
                send(res, 200, { alarms: rows });
                break;
            }
            case 'chargecycles': {
                const days = parseInt(req.query.days, 10) || 30;
                const rows = db.get().prepare('SELECT * FROM charge_cycles WHERE startTime >= ? ORDER BY startTime DESC').all(now - days * 86400 * 1000);
                send(res, 200, { cycles: rows });
                break;
            }
            case 'capacity': {
                if (method === 'POST') {
                    const body = getBody(req);
                    if (body.enabled !== undefined) db.setSetting('capLearningEnabled', body.enabled ? 'true' : 'false');
                    send(res, 200, { ok: true });
                } else {
                    const rows = db.get().prepare('SELECT * FROM capacity_learning ORDER BY id DESC LIMIT 20').all();
                    send(res, 200, {
                        enabled: db.getSetting('capLearningEnabled', 'true') === 'true',
                        calibratedCapacity: parseFloat(db.getSetting('calibratedCapacity', '0') || '0'),
                        learnPhase: db.getSetting('capPhase', 'idle'),
                        learnHistory: rows,
                    });
                }
                break;
            }
            case 'authcheck': {
                send(res, 200, { ok: config.auth.enabled ? auth.check(tokenOf(req)) : true });
                break;
            }
            default:
                send(res, 404, { error: '未知 action: ' + action });
        }
    }

    // 供 server.js 的 ingest 处理直接调用 (HTTP 兼容)
    ctx.handleUplink = ctx.handleUplink || null;
};
