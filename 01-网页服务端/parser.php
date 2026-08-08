<?php
// 防直接访问: 仅允许通过入口文件 require 加载
if (!defined('JK_INCLUDED')) { http_response_code(403); exit('forbidden'); }
/**
 * BMS 原始帧解析（移植自参考代码1 的 bms-parser.js）+ 文本消息分发
 * 兼容：
 *  - hex 连零压缩（!n!）还原
 *  - 24S / 32S 自动识别
 *  - 0x01 设置帧 / 0x02 电芯信息帧
 *  - 文本消息：gps / lbs / bms:raw / b:r / blestatus / csq / relay / switch / sendfreq / autobal
 */

// PHP 7.4 兼容：str_starts_with / str_ends_with / str_contains（PHP 8.0+ 内置）
if (!function_exists('str_starts_with')) {
    function str_starts_with(string $haystack, string $needle): bool
    {
        return $needle === '' || strncmp($haystack, $needle, strlen($needle)) === 0;
    }
}
if (!function_exists('str_ends_with')) {
    function str_ends_with(string $haystack, string $needle): bool
    {
        return $needle === '' || substr($haystack, -strlen($needle)) === $needle;
    }
}
if (!function_exists('str_contains')) {
    function str_contains(string $haystack, string $needle): bool
    {
        return $needle === '' || strpos($haystack, $needle) !== false;
    }
}

/** hex → bytes，支持 !n! 连零压缩 */
function hexToBytes(string $hex): array
{
    $bytes = [];
    $len   = strlen($hex);
    $i     = 0;
    while ($i < $len) {
        if ($hex[$i] === '!' && $i + 1 < $len) {
            $end = strpos($hex, '!', $i + 1);
            if ($end !== false && $end > $i + 1) {
                $count = intval(substr($hex, $i + 1, $end - $i - 1));
                if ($count > 0) {
                    for ($j = 0; $j < $count; $j++) {
                        $bytes[] = 0;
                    }
                }
                $i = $end + 1;
                continue;
            }
        }
        if ($i + 1 < $len) {
            $bytes[] = hexdec(substr($hex, $i, 2));
            $i += 2;
        } else {
            break;
        }
    }
    return $bytes;
}

function get16(array $b, int $i): int
{
    if ($i + 1 >= count($b)) return 0;
    return (($b[$i + 1] << 8) | $b[$i]) & 0xFFFF;
}

function get32(array $b, int $i): int
{
    if ($i + 3 >= count($b)) return 0;
    // 显式 &0xFF 保证 32 位 PHP 下高位字节不溢出为负
    return (($b[$i] & 0xFF) | (($b[$i + 1] & 0xFF) << 8) | (($b[$i + 2] & 0xFF) << 16) | (($b[$i + 3] & 0xFF) << 24)) & 0xFFFFFFFF;
}

function getS16(array $b, int $i): int
{
    $v = get16($b, $i);
    return $v & 0x8000 ? $v - 0x10000 : $v;
}

function getS32(array $b, int $i): int
{
    $v = get32($b, $i);
    return $v & 0x80000000 ? $v - 0x100000000 : $v;
}

function countBits(int $n): int
{
    $c = 0;
    for ($i = 0; $i < 32; $i++) {
        if ($n & (1 << $i)) $c++;
    }
    return $c;
}

/**
 * 解析 BMS 原始帧入口（校验帧头 + CRC，按帧类型分发）
 * @return array|null
 */
function parseBMSRaw(string $hex): ?array
{
    $b = hexToBytes($hex);
    if (count($b) < 130) return null;
    if ($b[0] !== 0x55 || $b[1] !== 0xAA || $b[2] !== 0xEB || $b[3] !== 0x90) return null;

    // CRC：整帧累加和取低 8 位（老版 300B 帧；新版无 CRC 帧跳过校验）
    $crc = 0;
    for ($i = 0; $i < count($b) - 1; $i++) {
        $crc = ($crc + $b[$i]) & 0xFF;
    }
    $crcOk = ($crc === $b[count($b) - 1]);
    if (!$crcOk && count($b) >= 290) {
        // 老版 300 字节帧：CRC 不匹配则丢弃
        return null;
    }

    $ftype = $b[4];
    if ($ftype === 0x02) return parseJK02CellInfo($b);
    if ($ftype === 0x01) return parseJK01Settings($b);
    return null;
}

/** 0x01 设置帧：开关状态 + 额定容量 */
function parseJK01Settings(array $b): array
{
    $chargeMOS    = ($b[118] ?? 0) > 0;
    $dischargeMOS = ($b[122] ?? 0) > 0;
    $balancerOn   = ($b[126] ?? 0) > 0;
    $capNominal   = get32($b, 130) * 0.001;
    $capValid     = $capNominal >= 1 && $capNominal <= 1000;

    return [
        'frameType'      => 0x01,
        'chargeMOS'      => $chargeMOS,
        'dischargeMOS'   => $dischargeMOS,
        'balancerOn'     => $balancerOn,
        'capNominal'     => $capValid ? round($capNominal, 2) : null,
        'capNominalValid'=> $capValid,
    ];
}

/** 0x02 电芯信息帧：24S/32S 自动识别 + 全字段解析 */
function parseJK02CellInfo(array $b): array
{
    $bm24 = get32($b, 54);
    $bm32 = get32($b, 70);
    $n24  = countBits($bm24);
    $n32  = countBits($bm32);

    $avg24 = get16($b, 58) * 0.001;
    $avg32 = get16($b, 74) * 0.001;
    $isAvg24Valid = $avg24 >= 2.0 && $avg24 <= 5.0;
    $isAvg32Valid = $avg32 >= 2.0 && $avg32 <= 5.0;

    if ($isAvg32Valid && !$isAvg24Valid) {
        $is32S = true; $cellCount = $n32; $totalCells = 32; $offset = 16; $off2 = 32;
    } elseif ($isAvg24Valid && !$isAvg32Valid) {
        $is32S = false; $cellCount = $n24; $totalCells = 24; $offset = 0; $off2 = 0;
    } elseif ($n32 > 0 && $n24 === 0) {
        $is32S = true; $cellCount = $n32; $totalCells = 32; $offset = 16; $off2 = 32;
    } elseif ($n24 > 0 && $n32 === 0) {
        $is32S = false; $cellCount = $n24; $totalCells = 24; $offset = 0; $off2 = 0;
    } else {
        // 模糊情况：用电压有效数与位图数量匹配
        $voltCells = 0;
        for ($i = 0; $i < 32; $i++) {
            $v = get16($b, 6 + $i * 2) * 0.001;
            if ($v >= 2.0 && $v <= 5.0) $voltCells++;
        }
        if (abs($n32 - $voltCells) <= abs($n24 - $voltCells)) {
            $is32S = true; $cellCount = $n32; $totalCells = 32; $offset = 16; $off2 = 32;
        } else {
            $is32S = false; $cellCount = $n24; $totalCells = 24; $offset = 0; $off2 = 0;
        }
    }
    if ($cellCount === 0) $cellCount = 16;

    // 单体电压
    $cellVoltages = [];
    for ($i = 0; $i < $totalCells; $i++) {
        $cellVoltages[] = round(get16($b, 6 + $i * 2) * 0.001, 3);
    }

    // 线束内阻
    $cellResist = [];
    for ($i = 0; $i < $totalCells; $i++) {
        $cellResist[] = round(get16($b, 64 + $offset + $i * 2) * 0.001, 3);
    }

    $totalVoltage = round(get32($b, 118 + $off2) * 0.001, 3);
    $current      = round(getS32($b, 126 + $off2) * 0.001, 2);
    $power        = round($totalVoltage * $current, 1);

    $temp1    = round(getS16($b, 130 + $off2) * 0.1, 1);
    $temp2    = round(getS16($b, 132 + $off2) * 0.1, 1);
    $mosTemp  = $is32S
        ? round(getS16($b, 112 + $off2) * 0.1, 1)
        : round(getS16($b, 134 + $off2) * 0.1, 1);

    $errors       = $is32S ? get16($b, 134 + $off2) : get16($b, 136 + $off2);
    $balanceCurr  = round(getS16($b, 138 + $off2) * 0.001, 3);
    $balancingAction = $b[140 + $off2] ?? 0;
    $soc          = $b[141 + $off2] ?? 0;
    $capRemain    = round(get32($b, 142 + $off2) * 0.001, 2);
    $capNominal   = round(get32($b, 146 + $off2) * 0.001, 2);
    $cycleCount   = get32($b, 150 + $off2);
    $cycleCap     = round(get32($b, 154 + $off2) * 0.001, 3);
    $soh          = $b[158 + $off2] ?? 0;
    $runtimeSec   = get32($b, 162 + $off2);

    $days = intdiv($runtimeSec, 86400);
    $hrs  = intdiv($runtimeSec % 86400, 3600);
    $mins = intdiv($runtimeSec % 3600, 60);
    $runtimeStr = sprintf('%dd %02dh %02dm', $days, $hrs, $mins);

    $chargeMOS    = ($b[166 + $off2] ?? 0) > 0;
    $dischargeMOS = ($b[167 + $off2] ?? 0) > 0;
    $balancerOn   = ($b[169 + $off2] ?? 0) > 0;

    // 单体派生数据
    $validVoltages = array_values(array_filter($cellVoltages, fn($v) => $v > 0));
    $maxCellV = $validVoltages ? max($validVoltages) : null;
    $minCellV = $validVoltages ? min($validVoltages) : null;
    $avgCellV = $validVoltages ? round(array_sum($validVoltages) / count($validVoltages), 3) : null;
    $deltaCellV = ($maxCellV !== null && $minCellV !== null) ? round($maxCellV - $minCellV, 4) : null;

    return [
        'frameType'     => 0x02,
        'cellCount'     => $cellCount,
        'totalCells'    => $totalCells,
        'cellVoltages'  => $cellVoltages,
        'cellResist'    => $cellResist,
        'totalVoltage'  => $totalVoltage,
        'current'       => $current,
        'power'         => $power,
        'temp1'         => $temp1,
        'temp2'         => $temp2,
        'mosTemp'       => $mosTemp,
        'errors'        => $errors,
        'balanceCurr'   => $balanceCurr,
        'balancingAction'=> $balancingAction,
        'soc'           => $soc,
        'capRemain'     => $capRemain,
        'capNominal'    => $capNominal,
        'cycleCount'    => $cycleCount,
        'cycleCap'      => $cycleCap,
        'soh'           => $soh,
        'runtimeStr'    => $runtimeStr,
        'runtimeSec'    => $runtimeSec,
        'chargeMOS'     => $chargeMOS,
        'dischargeMOS'  => $dischargeMOS,
        'balancerOn'    => $balancerOn,
        'maxCellV'      => $maxCellV,
        'minCellV'      => $minCellV,
        'avgCellV'      => $avgCellV,
        'deltaCellV'    => $deltaCellV,
        'frameCounter'  => $b[5] ?? 0,
        'is32S'         => $is32S,
    ];
}

/**
 * 文本消息统一分发
 * @return array{type:?string,data:?array}
 */
function parseTextMessage(string $txt): array
{
    $txt = trim($txt);

    // ── GPS（兼容两种格式）──
    // 参考代码2（实测）：gps:fix,lonDir,lon,latDir,lat,speed
    // 参考代码1：gps:lon,lat,speed
    if (str_starts_with($txt, 'gps:')) {
        $p = explode(',', substr($txt, 4));
        if (count($p) >= 6) {
            $fix = trim($p[0]);
            if ($fix !== '1') {
                return ['type' => 'gps', 'data' => ['fix' => false]];
            }
            $lon   = floatval($p[2]);
            $lat   = floatval($p[4]);
            $speed = floatval($p[5] ?? 0);
            if ($lon == 0 || $lat == 0) {
                return ['type' => 'gps', 'data' => ['fix' => false]];
            }
            $data = ['fix' => true, 'lon' => $lon, 'lat' => $lat, 'speed' => $speed];
            // 可选扩展字段：gps:fix,lonDir,lon,latDir,lat,speed,sat,useSat,hdop
            if (isset($p[6]) && $p[6] !== '') $data['sat']    = intval($p[6]);
            if (isset($p[7]) && $p[7] !== '') $data['useSat'] = intval($p[7]);
            if (isset($p[8]) && $p[8] !== '') $data['hdop']   = floatval($p[8]);
            return ['type' => 'gps', 'data' => $data];
        }
        if (count($p) >= 2) {
            $lon = floatval($p[0]);
            $lat = floatval($p[1]);
            $speed = floatval($p[2] ?? 0);
            if ($lon <= 0 || $lat <= 0 || $lon >= 180 || $lat >= 90) {
                return ['type' => 'gps', 'data' => ['fix' => false]];
            }
            return ['type' => 'gps', 'data' => ['fix' => true, 'lon' => $lon, 'lat' => $lat, 'speed' => $speed]];
        }
        return ['type' => 'gps', 'data' => ['fix' => false]];
    }

    // ── LBS 基站定位 ──
    if (str_starts_with($txt, 'lbs:')) {
        $p = explode(',', substr($txt, 4));
        if (count($p) >= 2) {
            $lon = floatval($p[0]);
            $lat = floatval($p[1]);
            if ($lon > 0 && $lat > 0) {
                return ['type' => 'jzdw', 'data' => ['lon' => $lon, 'lat' => $lat]];
            }
        }
        return ['type' => null, 'data' => null];
    }

    // 简化定位 lon_lat
    if (preg_match('/^(\d+\.\d+)_(\d+\.\d+)$/', $txt, $m)) {
        $lon = floatval($m[1]);
        $lat = floatval($m[2]);
        if ($lon > 0 && $lat > 0 && $lon < 180 && $lat < 90) {
            return ['type' => 'jzdw', 'data' => ['lon' => $lon, 'lat' => $lat]];
        }
    }

    // ── BMS 原始帧 ──
    if (str_starts_with($txt, 'bms:raw,')) {
        return ['type' => 'bms', 'data' => parseBMSRaw(substr($txt, 8))];
    }
    if (str_starts_with($txt, 'b:r,')) {
        return ['type' => 'bms', 'data' => parseBMSRaw(substr($txt, 4))];
    }

    // ── 蓝牙状态 ──
    if (str_starts_with($txt, 'blestatus:')) {
        $p = explode(',', substr($txt, 10));
        return ['type' => 'blestatus', 'data' => [
            'state'     => $p[0] ?? 'unknown',
            'mac'       => $p[1] ?? '',
            'connected' => ($p[2] ?? '0') === '1',
        ]];
    }

    // ── 4G 信号 ──
    if (str_starts_with($txt, 'csq:')) {
        $v = intval(substr($txt, 4));
        return ['type' => 'csq', 'data' => ['csq' => $v]];
    }

    // ── 中继状态 ──
    if (str_starts_with($txt, 'relay:on'))  return ['type' => 'relay', 'data' => ['enabled' => true]];
    if (str_starts_with($txt, 'relay:off')) return ['type' => 'relay', 'data' => ['enabled' => false]];
    if (str_starts_with($txt, 'relaystatus:')) {
        $p = explode(',', substr($txt, 12));
        return ['type' => 'relay', 'data' => ['enabled' => ($p[0] ?? '') === 'on']];
    }

    // ── 开关执行结果 ──
    if (str_starts_with($txt, 'switch:')) {
        $parts = explode(':', substr($txt, 7));
        if (count($parts) >= 2) {
            $keyMap = ['charge' => 'chargeMOS', 'discharge' => 'dischargeMOS', 'balance' => 'balancerOn'];
            if (isset($keyMap[$parts[0]])) {
                return ['type' => 'switch', 'data' => [
                    'key'     => $keyMap[$parts[0]],
                    'enabled' => $parts[1] === 'on',
                    'error'   => $parts[1] === 'error',
                ]];
            }
        }
    }

    // ── 上报频率查询结果 ──
    if (str_starts_with($txt, 'sendfreq:')) {
        $result = [];
        foreach (explode(',', substr($txt, 9)) as $pair) {
            $kv = explode('=', $pair);
            if (count($kv) === 2) $result[$kv[0]] = intval($kv[1]);
        }
        return ['type' => 'sendfreq', 'data' => $result];
    }

    // ── 自动均衡状态 ──
    if (str_starts_with($txt, 'autobal:')) {
        $body = substr($txt, 8);
        if (str_contains($body, '=')) {
            $result = [];
            foreach (explode(',', $body) as $pair) {
                $kv = explode('=', $pair);
                if (count($kv) === 2) {
                    $k = trim($kv[0]);
                    $result[$k] = str_ends_with($k, '_volt') ? floatval($kv[1]) : (trim($kv[1]) === 'on');
                }
            }
            return ['type' => 'autobal', 'data' => $result];
        }
        $parts = explode(',', $body);
        if (count($parts) >= 2) {
            $result = [];
            if (in_array($parts[0], ['charge', 'static'], true)) {
                $result[$parts[0]] = $parts[1] === 'on';
                if (isset($parts[2])) $result[$parts[0] . '_volt'] = floatval($parts[2]);
                return ['type' => 'autobal', 'data' => $result];
            }
            if ($parts[0] === 'volt' && count($parts) >= 3) {
                $result[$parts[1] . '_volt'] = floatval($parts[2]);
                return ['type' => 'autobal', 'data' => $result];
            }
        }
        return ['type' => 'autobal', 'data' => []];
    }

    return ['type' => null, 'data' => null];
}
