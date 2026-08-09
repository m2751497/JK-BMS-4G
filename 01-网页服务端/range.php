<?php
// 防直接访问: 仅允许通过入口文件 require 加载
if (!defined('JK_INCLUDED')) { http_response_code(403); exit('forbidden'); }
/**
 * 里程计算与自动学习
 *  - 实时续航：剩余容量(Ah) × 里程基数(km/Ah)
 *  - 里程基数自动学习（随充随学）：任意一次充电周期都学，不要求满充 ——
 *    充电结束记录当时剩余容量作基准，下次充电开始时用 基准-当前容量 算消耗，
 *    该周期 GPS 轨迹距离 ÷ 消耗(Ah) = 实测 km/Ah，旧×0.7 + 实测×0.3 加权平滑更新
 *  - haversine 球面距离、WGS84→GCJ02 坐标转换
 */

require_once __DIR__ . '/db.php';

/** haversine 球面距离（km） */
function haversine(float $lat1, float $lon1, float $lat2, float $lon2): float
{
    $R = 6371.0;
    $dLat = deg2rad($lat2 - $lat1);
    $dLon = deg2rad($lon2 - $lon1);
    $a = sin($dLat / 2) ** 2 +
         cos(deg2rad($lat1)) * cos(deg2rad($lat2)) * sin($dLon / 2) ** 2;
    return $R * 2 * atan2(sqrt($a), sqrt(1 - $a));
}

/** 当前里程基数（km/Ah），从 app_settings 读取 */
function getRangeFactor(): float
{
    $cfg = require __DIR__ . '/config.php';
    $v = getAppSetting('rangeFactor');
    return $v !== null ? floatval($v) : $cfg['rangeFactorDefault'];
}

/** 实时续航估算（km），无剩余容量返回 null */
function calcRangeKm(?float $capRemain): ?float
{
    if ($capRemain === null || $capRemain <= 0) return null;
    return round($capRemain * getRangeFactor(), 1);
}

/**
 * 里程基数自动学习
 * ⚠ WebHook 架构注意：每次 ingest 都是独立 HTTP 请求/进程，不能用 static 存状态，
 *    学习状态（阶段/起点/放电标记）全部持久化到 SQLite app_settings，跨请求生效。
 * @param array $data BMS 解析数据
 * @return void
 */
function updateRangeLearning(array $data): void
{
    // 学习开关（网页「设置」页可在线关闭，实时生效）
    if (getAppSetting('learningEnabled', 'true') !== 'true') {
        setAppSetting('learnWasCharging', 'false');
        return;
    }

    $current = $data['current'] ?? 0;
    // 极空 BMS 协议约定：放电为负 / 充电为正（与网页显示一致）
    $isCharging = $current > 0.1;    // 充电 = 正电流（阈值 0.1A，涓流也算）
    $capRemain  = (float)($data['capRemain'] ?? 0);
    $now        = time() * 1000;

    $wasCharging = getAppSetting('learnWasCharging', 'false') === 'true';
    $lastEndCap  = (float)getAppSetting('learnLastEndCap', '0');
    $lastEndTs   = (int)getAppSetting('learnEndTs', '0');

    // ★v2.14 随充随学：任何一次充电周期都学习，不要求满充。
    //   充电结束 → 记录当时剩余容量作基准；下次充电开始 → 用基准-当前容量算消耗并结算。

    // ① 充电开始（非充→充）：用上次充电结束的容量基准结算
    if ($isCharging && !$wasCharging) {
        if ($lastEndCap > 0 && $lastEndTs > 0) {
            $capUsed = $lastEndCap - $capRemain;   // 上次充完 → 本次插电之间的消耗
            if ($capUsed > 0.5) {
                calculateLearnedFactor($lastEndTs, $now, $capUsed);
            } elseif ($capUsed > 0) {
                ingestLog("[里程学习] 消耗过少(" . round($capUsed, 2) . "Ah), 跳过本次计算");
            }
        }
        // 本次充电开始后，等待充电结束再建立新基准
        setAppSetting('learnLastEndCap', '0');
        setAppSetting('learnEndTs', '0');
    }

    // ② 充电结束（充→非充）：记录充电结束容量作为下次结算基准
    if (!$isCharging && $wasCharging) {
        setAppSetting('learnLastEndCap', (string)$capRemain);
        setAppSetting('learnEndTs', (string)$now);
        ingestLog("[里程学习] 充电结束, 记录基准容量=" . round($capRemain, 2)
            . "Ah (下次充电开始时结算此周期 km/Ah)");
    }

    setAppSetting('learnWasCharging', $isCharging ? 'true' : 'false');
}

/** 计算学习因子：GPS 轨迹距离 / 消耗容量，加权更新 */
function calculateLearnedFactor(int $startTs, int $endTs, float $capUsed): void
{
    $st = db()->prepare(
        'SELECT lat, lon FROM gps_data WHERE ts >= ? AND ts <= ? AND fix = 1 ORDER BY ts ASC'
    );
    $st->execute([$startTs, $endTs]);
    $rows = $st->fetchAll(PDO::FETCH_ASSOC);

    if (count($rows) < 2) {
        ingestLog("[里程学习] GPS 数据不足(" . count($rows) . "点), 跳过本次计算");
        return;
    }

    $distance = 0.0;
    for ($i = 1; $i < count($rows); $i++) {
        $distance += haversine(
            floatval($rows[$i - 1]['lat']), floatval($rows[$i - 1]['lon']),
            floatval($rows[$i]['lat']),    floatval($rows[$i]['lon'])
        );
    }

    if ($distance < 0.2) {
        ingestLog("[里程学习] 距离过短(" . round($distance, 2) . "km), 跳过本次计算");
        return;
    }

    $learnedFactor = round($distance / $capUsed, 2);
    $oldFactor = getRangeFactor();
    $newFactor = round($oldFactor * 0.7 + $learnedFactor * 0.3, 2);
    setAppSetting('rangeFactor', (string)$newFactor);

    // 记录学习历史
    $ins = db()->prepare(
        'INSERT INTO range_learning (ts, startTs, endTs, distance, capUsed, learnedFactor, oldFactor, newFactor)
         VALUES (?, ?, ?, ?, ?, ?, ?, ?)'
    );
    $ins->execute([
        time() * 1000, $startTs, $endTs,
        round($distance, 2), round($capUsed, 2),
        $learnedFactor, $oldFactor, $newFactor,
    ]);

    ingestLog("[里程学习] 完成: 距离=" . round($distance, 2)
       . "km 消耗={$capUsed}Ah 学习基数={$learnedFactor} 新基数={$newFactor}(旧={$oldFactor})");
}

/**
 * WGS84 → GCJ02 坐标转换（国内地图对齐）
 * @return array{0:float,1:float} [lat, lng]
 */
function wgs84ToGcj02(float $wgsLat, float $wgsLng): array
{
    $a = 6378245.0;
    $ee = 0.00669342162296594323;

    $transformLat = function (float $x, float $y) use ($a, $ee): float {
        $ret = -100.0 + 2.0 * $x + 3.0 * $y + 0.2 * $y * $y + 0.1 * $x * $y + 0.2 * sqrt(abs($x));
        $ret += (20.0 * sin(6.0 * $x * M_PI) + 20.0 * sin(2.0 * $x * M_PI)) * 2.0 / 3.0;
        $ret += (20.0 * sin($y * M_PI) + 40.0 * sin($y / 3.0 * M_PI)) * 2.0 / 3.0;
        $ret += (160.0 * sin($y / 12.0 * M_PI) + 320.0 * sin($y * M_PI / 30.0)) * 2.0 / 3.0;
        return $ret;
    };

    $transformLng = function (float $x, float $y) use ($a, $ee): float {
        $ret = 300.0 + $x + 2.0 * $y + 0.1 * $x * $x + 0.1 * $x * $y + 0.1 * sqrt(abs($x));
        $ret += (20.0 * sin(6.0 * $x * M_PI) + 20.0 * sin(2.0 * $x * M_PI)) * 2.0 / 3.0;
        $ret += (20.0 * sin($x * M_PI) + 40.0 * sin($x / 3.0 * M_PI)) * 2.0 / 3.0;
        $ret += (150.0 * sin($x / 12.0 * M_PI) + 300.0 * sin($x / 30.0 * M_PI)) * 2.0 / 3.0;
        return $ret;
    };

    // 超出中国范围直接返回
    if ($wgsLng < 72.004 || $wgsLng > 137.8347 || $wgsLat < 0.8293 || $wgsLat > 55.8271) {
        return [$wgsLat, $wgsLng];
    }

    $dLat = $transformLat($wgsLng - 105.0, $wgsLat - 35.0);
    $dLng = $transformLng($wgsLng - 105.0, $wgsLat - 35.0);
    $radLat = $wgsLat / 180.0 * M_PI;
    $magic = sin($radLat);
    $magic = 1 - $ee * $magic * $magic;
    $sqrtMagic = sqrt($magic);
    $dLat = ($dLat * 180.0) / (($a * (1 - $ee)) / ($magic * $sqrtMagic) * M_PI);
    $dLng = ($dLng * 180.0) / ($a / $sqrtMagic * cos($radLat) * M_PI);

    return [round($wgsLat + $dLat, 6), round($wgsLng + $dLng, 6)];
}
