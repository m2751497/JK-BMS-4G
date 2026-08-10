<?php
define('JK_INCLUDED', true); // 标记为入口: 允许 require 内部文件
/**
 * REST API（网页看板轮询接口，与 index.php 同目录，放网站根目录即可）
 *  GET  api.php?action=latest             最新 BMS + GPS + 状态 + 续航
 *  GET  api.php?action=heartbeat&uid=xxx  网页心跳（在线检测）
 *  GET  api.php?action=track&from=&to=    GPS 轨迹（含 GCJ02 转换 + 总里程）
 *  GET  api.php?action=range              里程配置查询
 *  POST api.php?action=range              设置里程基数 / 学习开关
 */

header('Content-Type: application/json; charset=utf-8');
header('Access-Control-Allow-Origin: *');
header('Access-Control-Allow-Methods: GET, POST, OPTIONS');
header('Access-Control-Allow-Headers: Content-Type');
// 实时数据接口一律禁止缓存（防浏览器/代理缓存旧数据）
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');
header('Pragma: no-cache');
header('Expires: 0');

if ($_SERVER['REQUEST_METHOD'] === 'OPTIONS') {
    http_response_code(204);
    exit;
}

require_once __DIR__ . '/db.php';
require_once __DIR__ . '/range.php';
require_once __DIR__ . '/parser.php';
require_once __DIR__ . '/ingest.php';

$path = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH);
$action = $_GET['action'] ?? basename(rtrim($path, '/'));
if ($action === 'api.php') {
    // 兜底：路径型 /api.php?action=xxx
    $action = $_GET['action'] ?? '';
}

// ==================== WebHook 接收端点（无常驻进程方案） ====================
// EMQX 规则引擎把 T 主题消息 POST 到这里（服务器→服务器，用令牌鉴权，不走网页登录）
// EMQX WebHook 转发入口：action=ingest 或 带 X-Ingest-Token 头的请求（兼容 EMQX 对 query 的处理差异）
if ($action === 'ingest' || !empty($_SERVER['HTTP_X_INGEST_TOKEN'])) {
    $cfg = require __DIR__ . '/config.php';
    if (empty($cfg['webhook']['enabled'])) {
        http_response_code(404);
        echo '{}';
        exit;
    }
    // 令牌校验：请求头 X-Ingest-Token 或 ?token=
    $got = $_SERVER['HTTP_X_INGEST_TOKEN'] ?? ($_GET['token'] ?? '');
    if (!hash_equals((string)($cfg['webhook']['token'] ?? ''), (string)$got)) {
        http_response_code(403);
        echo json_encode(['error' => 'forbidden']);
        exit;
    }
    // 取 payload：EMQX WebHook 默认 POST JSON {payload, topic, ...}；兼容纯文本 POST
    // 防 DoS: 限制请求体大小 (正常 BMS 帧 < 1KB, EMQX WebHook JSON < 8KB)
    $cl = (int)($_SERVER['CONTENT_LENGTH'] ?? 0);
    if ($cl > 64 * 1024) {
        http_response_code(413);
        echo json_encode(['error' => 'payload too large']);
        exit;
    }
    $raw = file_get_contents('php://input') ?: '';
    // 二次校验: Content-Length 头可能被伪造或 chunked 传输绕过, 按实际读取长度兜底
    if (strlen($raw) > 64 * 1024) {
        http_response_code(413);
        echo json_encode(['error' => 'payload too large']);
        exit;
    }
    $body = json_decode($raw, true);
    $payload = is_array($body) ? ($body['payload'] ?? null) : ($raw !== '' ? $raw : null);
    if (is_array($payload)) $payload = json_encode($payload, JSON_UNESCAPED_UNICODE);
    $payload = trim((string)$payload);
    if ($payload !== '') ingestText($payload);
    echo json_encode(['ok' => true]);
    exit;
}

// ==================== 定时在线检测端点（cron 兜底） ====================
// 虚拟主机 cron 每分钟调一次：wget -q -O- "http://域名/api.php?action=checkmode&token=xxx"
// 保证即使没有网页心跳、没有设备数据，在线状态也能收敛（网页关闭后降 track）
if ($action === 'checkmode') {
    $cfg = require __DIR__ . '/config.php';
    if (empty($cfg['webhook']['enabled'])) {
        http_response_code(404);
        echo '{}';
        exit;
    }
    $got = $_GET['token'] ?? '';
    if (!hash_equals((string)($cfg['webhook']['token'] ?? ''), (string)$got)) {
        http_response_code(403);
        echo json_encode(['error' => 'forbidden']);
        exit;
    }
    checkAndSendMode();
    echo json_encode(['ok' => true]);
    exit;
}

// 登录校验：未登录时接口返回 401
require_once __DIR__ . '/auth.php';
if (!authCheck()) {
    http_response_code(401);
    echo json_encode(['error' => '未登录']);
    exit;
}

/** 防 CSRF：登录后的 POST 接口校验 Origin/Referer 必须与本站同源 */
function checkSameOrigin(): void
{
    $origin  = $_SERVER['HTTP_ORIGIN'] ?? '';
    $referer = $_SERVER['HTTP_REFERER'] ?? '';
    $toCheck = $origin !== '' ? $origin : $referer;
    if ($toCheck === '') return; // 无来源头（curl/CLI/同域表单）放行
    $u = @parse_url($toCheck);
    if ($u === false || empty($u['host'])) {
        http_response_code(403);
        echo json_encode(['error' => 'forbidden']);
        exit;
    }
    $srcHost = strtolower((string)$u['host']);
    $srcPort = isset($u['port']) ? (string)$u['port'] : '';
    $host = strtolower($_SERVER['HTTP_HOST'] ?? '');
    // 拆分 HTTP_HOST 中的端口（默认端口 80/443 与无端口视为一致）
    $hostName = $host;
    $hostPort = '';
    if (strpos($host, ':') !== false) {
        [$hostName, $hostPort] = explode(':', $host, 2);
    }
    $defaultPort = ($u['scheme'] ?? 'http') === 'https' ? '443' : '80';
    $portOk = ($srcPort === '' || $srcPort === $defaultPort || $srcPort === $hostPort);
    if ($srcHost !== $hostName || !$portOk) {
        http_response_code(403);
        echo json_encode(['error' => 'forbidden']);
        exit;
    }
}

switch ($action) {
    // ==================== 最新数据 ====================
    case 'latest': {
        $cache = readCache();
        $out = [
            'ts'         => $cache['ts'] ?? null,
            'lastUpdate' => $cache['lastUpdate'] ?? null,
            'online'     => $cache['online'] ?? 0,
            'mode'       => $cache['mode'] ?? 'track',
            'bms'        => $cache['bms'] ?? null,
            'gps'        => $cache['gps'] ?? null,
            'status'     => $cache['status'] ?? null,
            'range'      => $cache['range'] ?? [
                'rangeFactor' => getRangeFactor(),
                'rangeKm'     => null,
            ],
            'settings'   => getSettings(),
        ];
        echo json_encode($out, JSON_UNESCAPED_UNICODE);
        break;
    }

    // ==================== 心跳 ====================
    case 'heartbeat': {
        $uid = $_GET['uid'] ?? 'web_' . md5($_SERVER['REMOTE_ADDR']);
        if (strlen($uid) > 64) $uid = substr($uid, 0, 64);
        $page = $_GET['page'] ?? 'gps';
        touchClient($uid, $page);
        checkAndSendMode(); // 在线检测 + 双模式下发（无常驻进程方案）
        echo json_encode(['ok' => true, 'ts' => time() * 1000]);
        break;
    }

    // ==================== GPS 轨迹 ====================
    case 'track': {
        $now = time() * 1000;
        // 默认按当天 0 点(本地时区)起算"今日轨迹"
        $from = isset($_GET['from']) ? intval($_GET['from']) : mktime(0, 0, 0) * 1000;
        $to   = isset($_GET['to']) ? intval($_GET['to']) : $now;

        $st = db()->prepare(
            'SELECT ts, lon, lat, speed FROM gps_data
             WHERE ts >= ? AND ts <= ? AND fix = 1 ORDER BY ts ASC'
        );
        $st->execute([$from, $to]);
        $rows = $st->fetchAll(PDO::FETCH_ASSOC);

        $points = [];
        $distance = 0.0;
        $prev = null;
        $speeds = [];
        $firstTs = null;
        $lastTs = null;
        foreach ($rows as $r) {
            $lat = floatval($r['lat']);
            $lon = floatval($r['lon']);
            $spd = floatval($r['speed']);
            if ($firstTs === null) $firstTs = intval($r['ts']);
            $lastTs = intval($r['ts']);
            $speeds[] = $spd;
            if ($prev !== null) {
                $distance += haversine($prev[0], $prev[1], $lat, $lon);
            }
            $prev = [$lat, $lon];
            [$gcjLat, $gcjLng] = wgs84ToGcj02($lat, $lon);
            $points[] = [
                'ts'    => intval($r['ts']),
                'lon'   => $lon,
                'lat'   => $lat,
                'speed' => $spd,
                'gcjLng'=> $gcjLng,
                'gcjLat'=> $gcjLat,
            ];
        }

        $count = count($speeds);
        echo json_encode([
            'count'       => $count,
            'distance'    => round($distance, 2),
            'avgSpeed'    => $count ? round(array_sum($speeds) / $count, 1) : 0,
            'maxSpeed'    => $count ? round(max($speeds), 1) : 0,
            'durationSec' => ($firstTs !== null && $lastTs !== null)
                ? max(0, intdiv($lastTs - $firstTs, 1000)) : 0,
            'points'      => $points,
        ], JSON_UNESCAPED_UNICODE);
        break;
    }

    // ==================== 轨迹日期列表 ====================
    case 'trackdates': {
        $st = db()->query(
            "SELECT DISTINCT date(ts / 1000, 'unixepoch', 'localtime') AS d
             FROM gps_data ORDER BY d DESC"
        );
        $dates = array_map(fn($r) => $r['d'], $st->fetchAll(PDO::FETCH_ASSOC));        echo json_encode(['dates' => $dates], JSON_UNESCAPED_UNICODE);
        break;
    }

    // ==================== 行程统计（按天 + 每次行程单独一条） ====================
    // 行程分割: 相邻入库点间隔 > 5 分钟 → 新行程 (停车/关机断开)
    // 行驶时长: 仅累加"相邻点位移 > 20m"的移动段时间 (排除停车漂移导致的虚增)
    case 'trips': {
        $date = isset($_GET['date'])
            ? preg_replace('/[^0-9\-]/', '', $_GET['date'])
            : date('Y-m-d');
        $st = db()->prepare(
            "SELECT ts, lon, lat, speed FROM gps_data
             WHERE date(ts / 1000, 'unixepoch', 'localtime') = :d AND fix = 1
             ORDER BY ts ASC"
        );
        $st->execute([':d' => $date]);
        $rows = $st->fetchAll(PDO::FETCH_ASSOC);

        $SPLIT_GAP  = 300;      // 相邻点间隔 > 300s(5分钟) → 新行程
        $MOVE_DIST  = 0.020;    // 位移 > 20m 才算移动段 (km), 计入行驶时长

        $trips = [];
        $cur = null;
        $prevTs = null; $prevLat = null; $prevLon = null;

        foreach ($rows as $r) {
            $ts  = intdiv(intval($r['ts']), 1000);
            $lat = floatval($r['lat']);
            $lon = floatval($r['lon']);
            $spd = floatval($r['speed']);

            if ($cur === null || ($prevTs !== null && $ts - $prevTs > $SPLIT_GAP)) {
                if ($cur !== null) $trips[] = $cur;
                $cur = ['startTs' => $ts, 'endTs' => $ts, 'dist' => 0.0, 'dur' => 0, 'spds' => [$spd], 'count' => 1];
                $prevTs = $ts; $prevLat = $lat; $prevLon = $lon;
                continue;
            }

            $cur['endTs'] = $ts;
            $cur['spds'][] = $spd;
            $cur['count']++;
            $d = haversine($prevLat, $prevLon, $lat, $lon);
            $cur['dist'] += $d;
            if ($d > $MOVE_DIST) {
                $cur['dur'] += ($ts - $prevTs);  // 移动段时间才计入行驶时长
            }
            $prevTs = $ts; $prevLat = $lat; $prevLon = $lon;
        }
        if ($cur !== null) $trips[] = $cur;

        // 丢弃 < 2 点的碎片行程
        $trips = array_values(array_filter($trips, fn($t) => $t['count'] >= 2));

        $out = [];
        foreach ($trips as $t) {
            $distKm = round($t['dist'], 2);
            $durSec = $t['dur'];
            $count  = $t['count'];
            $out[] = [
                'startTs'     => $t['startTs'],
                'endTs'       => $t['endTs'],
                'distanceKm'  => $distKm,
                'durationSec' => $durSec,
                'avgSpeed'    => $durSec > 0 ? round($distKm / ($durSec / 3600.0), 1) : 0,
                'maxSpeed'    => $count ? round(max($t['spds']), 1) : 0,
                'count'       => $count,
            ];
        }
        echo json_encode(['date' => $date, 'trips' => $out], JSON_UNESCAPED_UNICODE);
        break;
    }

    // ==================== 按日期/时间范围轨迹回放 ====================
    case 'replay': {
        // 支持按时间范围 (from/to, 毫秒) 回放单次行程; 无 from/to 时按日期回放全天
        if (isset($_GET['from']) || isset($_GET['to'])) {
            $from = isset($_GET['from']) ? intval($_GET['from']) : 0;
            $to   = isset($_GET['to']) ? intval($_GET['to']) : (time() * 1000);
            $st = db()->prepare(
                'SELECT ts, lon, lat, speed FROM gps_data
                 WHERE ts >= ? AND ts <= ? AND fix = 1 ORDER BY ts ASC'
            );
            $st->execute([$from, $to]);
            $date = isset($_GET['date']) ? $_GET['date'] : '';
        } else {
            $date = isset($_GET['date'])
                ? preg_replace('/[^0-9\-]/', '', $_GET['date'])
                : date('Y-m-d');
            $st = db()->prepare(
                "SELECT ts, lon, lat, speed FROM gps_data
                 WHERE date(ts / 1000, 'unixepoch', 'localtime') = :d
                 ORDER BY ts ASC"
            );
            $st->execute([':d' => $date]);
        }
        $rows = $st->fetchAll(PDO::FETCH_ASSOC);

        $points = [];
        foreach ($rows as $r) {
            $lat = floatval($r['lat']);
            $lon = floatval($r['lon']);
            [$gcjLat, $gcjLng] = wgs84ToGcj02($lat, $lon);
            $points[] = [
                't'      => intdiv(intval($r['ts']), 1000), // 秒
                'lat'    => $lat,
                'lon'    => $lon,
                'speed'  => floatval($r['speed']),
                'gcjLat' => $gcjLat,
                'gcjLng' => $gcjLng,
            ];
        }
        echo json_encode([
            'date'   => $date,
            'count'  => count($points),
            'points' => $points,
        ], JSON_UNESCAPED_UNICODE);
        break;
    }

    // ==================== 里程配置 ====================
    case 'range': {
        if ($_SERVER['REQUEST_METHOD'] === 'POST') {
            checkSameOrigin(); // 防 CSRF
            $body = json_decode(file_get_contents('php://input'), true) ?: [];
            $changed = [];

            if (isset($body['rangeFactor'])) {
                $f = floatval($body['rangeFactor']);
                if ($f >= 0.1 && $f <= 50) {
                    setAppSetting('rangeFactor', (string)round($f, 2));
                    $changed['rangeFactor'] = $f;
                } else {
                    http_response_code(400);
                    echo json_encode(['error' => 'rangeFactor 需在 0.1 ~ 50 之间']);
                    exit;
                }
            }
            if (isset($body['learningEnabled'])) {
                setAppSetting('learningEnabled', $body['learningEnabled'] ? 'true' : 'false');
                $changed['learningEnabled'] = (bool)$body['learningEnabled'];
            }

            echo json_encode(['ok' => true, 'changed' => $changed]);
            break;
        }

        $learning = getAppSetting('learningEnabled', 'true');
        // 学习状态（随充随学 v2.14）: charging=充电中 / ready=基准就绪 / no_base=等待首次充电
        $wasCharging = getAppSetting('learnWasCharging', 'false') === 'true';
        $hasBase = (float)getAppSetting('learnLastEndCap', '0') > 0;
        $phase = $wasCharging ? 'charging' : ($hasBase ? 'ready' : 'no_base');
        $history = [];
        try {
            $st = db()->query(
                'SELECT ts, distance, capUsed, learnedFactor, newFactor FROM range_learning ORDER BY ts DESC LIMIT 5'
            );
            $history = $st->fetchAll(PDO::FETCH_ASSOC);
        } catch (Exception $e) { /* 表不存在(未学过)时忽略 */ }
        echo json_encode([
            'rangeFactor'     => getRangeFactor(),
            'learningEnabled' => $learning === 'true',
            'learnPhase'      => $phase,
            'learnHistory'    => $history,
        ], JSON_UNESCAPED_UNICODE);
        break;
    }

    // ==================== 可调设置（网页刷新/上报间隔 + MQTT/高德/账号） ====================
    // ==================== 远程重启设备（下行链路通时生效） ====================
    case 'reboot': {
        if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
            http_response_code(405);
            echo json_encode(['error' => 'method not allowed']);
            break;
        }
        checkSameOrigin(); // 防 CSRF
        sendModeCommand('reboot');  // → EMQX R 主题 → DTU → ESP32 串口
        echo json_encode(['ok' => true]);
        break;
    }

    case 'settings': {
        if ($_SERVER['REQUEST_METHOD'] === 'POST') {
            checkSameOrigin(); // 防 CSRF
            $body = json_decode(file_get_contents('php://input'), true) ?: [];
            $changed = [];

            // ① 数字类：网页/上报间隔
            $ranges = [
                'webPollSec'        => [1, 30],
                'realtimeBmsSec'    => [1, 300],
                'realtimeGpsSec'    => [1, 300],
                'dataRetentionDays' => [0, 3650],
            ];
            foreach ($ranges as $k => $range) {
                if (isset($body[$k])) {
                    $v = intval($body[$k]);
                    if ($v < $range[0] || $v > $range[1]) {
                        http_response_code(400);
                        echo json_encode(['error' => "{$k} 需在 {$range[0]} ~ {$range[1]} 之间"]);
                        exit;
                    }
                    setSetting($k, $v);
                    $changed[$k] = $v;
                }
            }

            // ② 文本类：高德 key（登录账号在 ③ 统一处理）
            foreach (['amapKey', 'amapSecurity'] as $k) {
                if (isset($body[$k])) {
                    setAppSetting($k, trim((string)$body[$k]));
                    $changed[$k] = true;
                }
            }

            // ③ 登录账号/密码修改（任何账号相关变更都需验证当前密码，防已登录会话被静默篡改）
            $newUser = isset($body['authUsername']) ? trim((string)$body['authUsername']) : null;
            $newPass = (isset($body['authPassword']) && $body['authPassword'] !== '') ? (string)$body['authPassword'] : null;
            if ($newUser !== null || $newPass !== null) {
                // 用当前生效账号验证（不能用 body 里的新账号，否则改账号+改密码同时提交时必然失败）
                $curUser = authCreds()['username'];
                if (!authVerify($curUser, (string)($body['authCurrent'] ?? ''))) {
                    http_response_code(400);
                    echo json_encode(['error' => '当前密码错误，无法修改']);
                    exit;
                }
                if ($newUser !== null && $newUser !== '' && $newUser !== $curUser) {
                    if (strlen($newUser) < 2 || strlen($newUser) > 32) {
                        http_response_code(400);
                        echo json_encode(['error' => '账号长度需 2~32 位']);
                        exit;
                    }
                    setAppSetting('authUsername', $newUser);
                    $changed['authUsername'] = true;
                }
                if ($newPass !== null) {
                    if (strlen($newPass) < 6) {
                        http_response_code(400);
                        echo json_encode(['error' => '新密码至少 6 位']);
                        exit;
                    }
                    // 密码哈希存储（bcrypt），authVerify 兼容旧明文
                    setAppSetting('authPassword', password_hash($newPass, PASSWORD_DEFAULT));
                    $changed['authPassword'] = true;
                }
            }

            echo json_encode(['ok' => true, 'changed' => $changed]);
            break;
        }

        // GET：返回全部设置（含高德 / 账号）
        $cfg = require __DIR__ . '/config.php';
        $out = getSettings();
        $out['amapKey']     = getAppSetting('amapKey', '你的高德JSAPIKey');
        $out['amapSecurity']= getAppSetting('amapSecurity', '你的高德安全密钥');
        $out['authUsername']= getAppSetting('authUsername', $cfg['auth']['username']);
        echo json_encode($out, JSON_UNESCAPED_UNICODE);
        break;
    }

    // ★融合参考 Node 版: 告警列表
    case 'alarms': {
        $hours = min(max(intval($_GET['hours'] ?? 24), 1), 720);
        $level = $_GET['level'] ?? null;
        $q = 'SELECT * FROM alarms WHERE ts >= ?';
        $p = [time() * 1000 - $hours * 3600 * 1000];
        if ($level) { $q .= ' AND level = ?'; $p[] = $level; }
        $q .= ' ORDER BY id DESC LIMIT 100';
        $st = db()->prepare($q);
        $st->execute($p);
        echo json_encode(['data' => $st->fetchAll(PDO::FETCH_ASSOC)], JSON_UNESCAPED_UNICODE);
        break;
    }

    // ★融合参考 Node 版: 充电循环列表
    case 'chargecycles': {
        $days = min(max(intval($_GET['days'] ?? 30), 1), 365);
        $st = db()->prepare('SELECT * FROM charge_cycles WHERE startTime >= ? ORDER BY id DESC');
        $st->execute([time() * 1000 - $days * 86400 * 1000]);
        echo json_encode(['data' => $st->fetchAll(PDO::FETCH_ASSOC)], JSON_UNESCAPED_UNICODE);
        break;
    }

    // ★融合参考 Node 版: 容量校准状态/记录 + 开关
    case 'capacity': {
        if ($_SERVER['REQUEST_METHOD'] === 'POST') {
            checkSameOrigin();
            $body = json_decode(file_get_contents('php://input'), true) ?: [];
            if (isset($body['enabled'])) {
                setAppSetting('capLearningEnabled', !empty($body['enabled']) ? 'true' : 'false');
                echo json_encode(['ok' => true, 'enabled' => !empty($body['enabled'])]);
                break;
            }
            if (isset($body['reset'])) {
                setAppSetting('calibratedCapacity', '');
                echo json_encode(['ok' => true]);
                break;
            }
            http_response_code(400);
            echo json_encode(['error' => '未知操作']);
            break;
        }
        $st = db()->prepare('SELECT * FROM capacity_learning ORDER BY id DESC LIMIT 20');
        $st->execute();
        echo json_encode([
            'calibratedCapacity' => (float)getAppSetting('calibratedCapacity', '0'),
            'enabled'  => getAppSetting('capLearningEnabled', 'true') === 'true',
            'records'  => $st->fetchAll(PDO::FETCH_ASSOC),
        ], JSON_UNESCAPED_UNICODE);
        break;
    }

    default:
        http_response_code(404);
        echo json_encode(['error' => 'not found']);
}
