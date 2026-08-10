<?php
// 防直接访问: 仅允许通过入口文件 require 加载
if (!defined('JK_INCLUDED')) { http_response_code(403); exit('forbidden'); }
/**
 * SQLite 数据库：初始化 + PDO 访问封装
 * 表：bms_data / gps_data / app_settings / range_learning
 */

// 统一北京时间（虚拟主机常为 UTC，会导致网页时间与今日轨迹 0 点统计偏移）
if (!defined('JK_TZ_SET')) {
    define('JK_TZ_SET', true);
    date_default_timezone_set('Asia/Shanghai');
}

function db(): PDO
{
    static $pdo = null;
    if ($pdo === null) {
        $cfg = require __DIR__ . '/config.php';
        $dir = dirname($cfg['db']);
        if (!is_dir($dir)) {
            mkdir($dir, 0777, true);
        }
        $pdo = new PDO('sqlite:' . $cfg['db']);
        $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
        $pdo->exec('PRAGMA journal_mode = WAL;');
        // 完整性检查：数据库损坏时自动备份重建，避免 bridge 收到数据时崩溃循环
        $bad = false;
        try {
            $check = $pdo->query('PRAGMA integrity_check')->fetchColumn();
            $bad = ($check !== 'ok');
        } catch (Throwable $e) {
            $bad = true;
        }
        if ($bad) {
            $bak = $cfg['db'] . '.corrupted.' . date('Ymd_His');
            try { @copy($cfg['db'], $bak); } catch (Throwable $e) {}
            @unlink($cfg['db']); @unlink($cfg['db'] . '-wal'); @unlink($cfg['db'] . '-shm');
            $pdo = new PDO('sqlite:' . $cfg['db']);
            $pdo->setAttribute(PDO::ATTR_ERRMODE, PDO::ERRMODE_EXCEPTION);
            $pdo->exec('PRAGMA journal_mode = WAL;');
            error_log('[' . date('H:i:s') . "] ⚠ 数据库损坏, 已重建 (旧库备份: " . basename($bak) . ")");
        }
        initDB($pdo);
    }
    return $pdo;
}

function initDB(PDO $pdo): void
{
    $pdo->exec(
        "CREATE TABLE IF NOT EXISTS bms_data (
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
        )"
    );

    $pdo->exec(
        "CREATE TABLE IF NOT EXISTS gps_data (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts INTEGER NOT NULL,
            type TEXT NOT NULL,
            lon REAL,
            lat REAL,
            speed REAL,
            fix INTEGER
        )"
    );

    $pdo->exec(
        "CREATE TABLE IF NOT EXISTS app_settings (
            key TEXT PRIMARY KEY,
            value TEXT
        )"
    );

    $pdo->exec(
        "CREATE TABLE IF NOT EXISTS range_learning (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts INTEGER NOT NULL,
            startTs INTEGER,
            endTs INTEGER,
            distance REAL,
            capUsed REAL,
            learnedFactor REAL,
            oldFactor REAL,
            newFactor REAL
        );" .
        // ★融合 (参考 Node 版): 告警表 + 充电循环表 + 容量校准学习表 (SQLite exec 多条用分号分隔)
        "CREATE TABLE IF NOT EXISTS alarms (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts INTEGER NOT NULL,
            level TEXT NOT NULL,
            message TEXT NOT NULL
        );" .
        "CREATE TABLE IF NOT EXISTS charge_cycles (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            startTime INTEGER NOT NULL,
            endTime INTEGER,
            startSOC INTEGER,
            endSOC INTEGER,
            startCap REAL,
            endCap REAL,
            chargedAh REAL
        );" .
        "CREATE TABLE IF NOT EXISTS capacity_learning (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            ts INTEGER NOT NULL,
            startTs INTEGER,
            endTs INTEGER,
            capUsed REAL,
            oldCap REAL,
            newCap REAL
        )"
    );

    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_bms_data_ts ON bms_data(ts)');
    $pdo->exec('CREATE INDEX IF NOT EXISTS idx_gps_data_ts ON gps_data(ts)');
}

/**
 * 清理过期数据（保留最近 N 天，默认 90 天，config.php dataRetentionDays 可调）
 * 由 ingest 周期性触发（内部做节流：每 6 小时最多执行一次）
 */
function cleanOldData(): void
{
    static $lastClean = 0;
    $now = time();
    if ($now - $lastClean < 6 * 3600) return;
    $lastClean = $now;
    try {
        $settings = getSettings();
        $keepDays = (int)($settings['dataRetentionDays'] ?? 60);
        if ($keepDays <= 0) return;
        $cutoff = $now - $keepDays * 86400;
        $pdo = db();
        $pdo->prepare('DELETE FROM bms_data WHERE ts < ?')->execute([$cutoff]);
        $pdo->prepare('DELETE FROM gps_data WHERE ts < ?')->execute([$cutoff]);
        $pdo->prepare('DELETE FROM range_learning WHERE ts < ?')->execute([$cutoff]);
        // VACUUM 偶发碎片整理（SQLite 删除不会自动释放文件大小）
        $pdo->exec('VACUUM');
    } catch (Throwable $e) {
        // 清理失败不影响主流程
    }
}

/** 读取应用设置（键值） */
function getAppSetting(string $key, ?string $default = null): ?string
{
    $st = db()->prepare('SELECT value FROM app_settings WHERE key = ?');
    $st->execute([$key]);
    $v = $st->fetchColumn();
    return $v === false ? $default : $v;
}

/** 写入应用设置（键值，兼容旧版 SQLite：UPSERT 的 ON CONFLICT 需 SQLite≥3.24，虚拟主机可能更老） */
function setAppSetting(string $key, string $value): void
{
    $pdo = db();
    // 先 UPDATE，影响行数为 0（键不存在）再 INSERT
    $up = $pdo->prepare('UPDATE app_settings SET value = ? WHERE key = ?');
    $up->execute([$value, $key]);
    if ($up->rowCount() === 0) {
        $pdo->prepare('INSERT INTO app_settings (key, value) VALUES (?, ?)')->execute([$key, $value]);
    }
}

/* ==================== 共享缓存（桥接进程写 / 网页 API 读） ==================== */

function readJsonFile(string $path, array $default = []): array
{
    if (!is_file($path)) return $default;
    $raw = file_get_contents($path);
    $d = $raw ? json_decode($raw, true) : null;
    return is_array($d) ? $d : $default;
}

function writeJsonFile(string $path, array $data): void
{
    $dir = dirname($path);
    if (!is_dir($dir)) mkdir($dir, 0777, true);
    file_put_contents($path, json_encode($data, JSON_UNESCAPED_UNICODE), LOCK_EX);
}

/** 最新数据缓存 */
function readCache(): array
{
    return readJsonFile((require __DIR__ . '/config.php')['cacheFile']);
}

function writeCache(array $data): void
{
    writeJsonFile((require __DIR__ . '/config.php')['cacheFile'], $data);
}

/** 网页客户端心跳表 */
function readClients(): array
{
    return readJsonFile((require __DIR__ . '/config.php')['clientsFile']);
}

function writeClients(array $data): void
{
    writeJsonFile((require __DIR__ . '/config.php')['clientsFile'], $data);
}

/** 记录一次网页心跳（v2.19: 带当前页面 page，用于"仅 BMS 页在线才 realtime"） */
function touchClient(string $uid, string $page = 'gps'): void
{
    $clients = readClients();
    // 兼容旧格式：旧数据是纯时间戳数字 → 转新结构
    if (!is_array($clients[$uid] ?? null)) {
        $clients[$uid] = ['t' => $clients[$uid] ?? 0, 'page' => 'gps'];
    }
    $clients[$uid]['t'] = time() * 1000;
    $clients[$uid]['page'] = in_array($page, ['gps', 'bms', 'track', 'settings'], true) ? $page : 'gps';
    // 防止 uid 被刷爆：最多保留 50 条，超限按时间倒序剔除最旧的
    if (count($clients) > 50) {
        uasort($clients, fn($a, $b) => (is_array($b) ? $b['t'] : $b) <=> (is_array($a) ? $a['t'] : $a));
        $clients = array_slice($clients, 0, 50, true);
    }
    writeClients($clients);
}

/* ==================== 可调设置（网页设置页） ==================== */

/** 读取全部设置（默认值 + 已存值覆盖） */
function getSettings(): array
{
    $defaults = (require __DIR__ . '/config.php')['settings'];
    $out = [];
    foreach ($defaults as $k => $v) {
        $stored = getAppSetting('set_' . $k);
        $out[$k] = $stored !== null ? intval($stored) : $v;
    }
    return $out;
}

/** 写入单个设置 */
function setSetting(string $key, $value): void
{
    setAppSetting('set_' . $key, (string)intval($value));
}
