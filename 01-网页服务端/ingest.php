<?php
// 防直接访问: 仅允许通过入口文件 require 加载
if (!defined('JK_INCLUDED')) { http_response_code(403); exit('forbidden'); }
/**
 * WebHook 接收与公共处理逻辑（无常驻进程方案）
 * 职责：
 *  1. 接收 EMQX 规则引擎转发来的设备数据（HTTP POST → api.php?action=ingest）
 *  2. 解析文本（b:r / gps / blestatus / csq）→ 入库 + 缓存
 *  3. 日志写入 data/ingest.log
 *  4. 网页在线检测 → 通过 EMQX REST API 下发 mode 指令
 */

require_once __DIR__ . '/db.php';
require_once __DIR__ . '/parser.php';
require_once __DIR__ . '/range.php';

/** 写入接收日志（文件，替代 bridge 终端日志）；function_exists 防与其他文件的兜底定义冲突 */
if (!function_exists('ingestLog')) {
    function ingestLog(string $msg): void
    {
    static $lastTs = 0;
    // 节流：同一秒内高频消息只记一次，避免日志爆炸（realtime 下 2s 一条足够）
    $now = time();
    $cfg = require __DIR__ . '/config.php';
    $file = $cfg['ingestLog'] ?? __DIR__ . '/data/ingest.log';
    $dir = dirname($file);
    if (!is_dir($dir)) @mkdir($dir, 0777, true);
    // 日志轮转：超过 1MB 滚动为 .1（保留最近 2 份），防止无限增长
    static $checked = false;
    if (!$checked) {
        $checked = true;
        if (is_file($file) && filesize($file) > 1024 * 1024) {
            @unlink($file . '.1');
            @rename($file, $file . '.1');
        }
    }
    $line = '[' . date('Y-m-d H:i:s') . "] {$msg}\n";
    if ($now !== $lastTs) {
        @file_put_contents($file, $line, FILE_APPEND | LOCK_EX);
        $lastTs = $now;
    }
    }
}

/** BMS 数据入库 + 缓存 + 里程学习（原 bridge handleBMSData） */
function handleBMSData(array $data): void
{
    $now = time() * 1000;

    // ★修复: 0x01 帧(0x96 设置帧响应)不含 MOS/均衡状态 —— 原解析用推断偏移(118/122/126)
    //   读这些字节多为 0, 每轮询周期都来一帧, 会把 0x02 电芯帧(真实偏移 166/167/169)
    //   的正确 MOS/均衡状态覆盖成 false (表现为"均衡状态永远不变")。
    //   0x01 帧仅更新额定容量, 不写库、不覆盖其他字段。
    if (($data['frameType'] ?? null) === 0x01) {
        if (isset($data['capNominal'])) {
            $cache = readCache();
            if (isset($cache['bms']) && is_array($cache['bms'])) {
                $cache['bms']['capNominal'] = $data['capNominal'];
                $cache['lastUpdate'] = $now;
                writeCache($cache);
            }
        }
        return;
    }

    try {
        $ins = db()->prepare(
            'INSERT INTO bms_data (
                ts, totalVoltage, current, power, soc, soh, capRemain, capNominal, cycleCount,
                temp1, temp2, mosTemp, cellCount, cellVoltages, cellResist,
                chargeMOS, dischargeMOS, balancerOn
            ) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)'
        );
        $ins->execute([
            $now,
            $data['totalVoltage'] ?? null,
            $data['current'] ?? null,
            $data['power'] ?? null,
            $data['soc'] ?? null,
            $data['soh'] ?? null,
            $data['capRemain'] ?? null,
            $data['capNominal'] ?? null,
            $data['cycleCount'] ?? null,
            $data['temp1'] ?? null,
            $data['temp2'] ?? null,
            $data['mosTemp'] ?? null,
            $data['cellCount'] ?? null,
            json_encode($data['cellVoltages'] ?? [], JSON_UNESCAPED_UNICODE),
            json_encode($data['cellResist'] ?? [], JSON_UNESCAPED_UNICODE),
            !empty($data['chargeMOS']) ? 1 : 0,
            !empty($data['dischargeMOS']) ? 1 : 0,
            !empty($data['balancerOn']) ? 1 : 0,
        ]);

        // 里程学习
        updateRangeLearning($data);
    } catch (Throwable $e) {
        ingestLog("⚠ BMS 写库失败: " . $e->getMessage());
        return;
    }

    // 更新缓存
    $cache = readCache();
    $cache['ts'] = $now;
    $cache['bms'] = $data;
    $cache['bms']['ts'] = $now;
    $cache['range'] = [
        'rangeFactor' => getRangeFactor(),
        'rangeKm'     => calcRangeKm($data['capRemain'] ?? null),
    ];
    $cache['lastUpdate'] = $now;
    writeCache($cache);
}

/** GPS 入库（未定位/静止点过滤）+ 缓存（原 bridge handleGPSData） */
function handleGPSData(array $data, string $type): void
{
    $now = time() * 1000;
    $fix = !empty($data['fix']) ? 1 : 0;

    // 轨迹入库过滤：
    //  1) 未定位(fix=0)点不入库（缓存仍更新，前端显示"未定位"）
    //  2) 静止点去重：与最后入库点位移 < 5 米则跳过，避免车静止时轨迹点数无限增长
    $store = false;
    if ($fix && isset($data['lon'], $data['lat'])) {
        $store = true;
        try {
            $last = db()->query('SELECT lon, lat FROM gps_data ORDER BY id DESC LIMIT 1')->fetch(PDO::FETCH_ASSOC);
            if ($last) {
                $d = haversine(
                    (float)$last['lat'], (float)$last['lon'],
                    (float)$data['lat'], (float)$data['lon']
                ); // km
                if ($d < 0.005) $store = false; // 位移 < 5m → 静止，去重
            }
        } catch (Throwable $e) {
            $store = true; // 查询失败则保守入库
        }
    }

    if ($store) {
        try {
            $ins = db()->prepare(
                'INSERT INTO gps_data (ts, type, lon, lat, speed, fix) VALUES (?,?,?,?,?,?)'
            );
            $ins->execute([$now, $type, $data['lon'] ?? null, $data['lat'] ?? null, $data['speed'] ?? 0, $fix]);
        } catch (Throwable $e) {
            ingestLog("⚠ GPS 写库失败: " . $e->getMessage());
        }
    }

    // 缓存总是更新（前端实时状态 / 定位显示）
    $cache = readCache();
    $cache['gps'] = [
        'ts'     => $now,
        'lon'    => $data['lon'] ?? null,
        'lat'    => $data['lat'] ?? null,
        'speed'  => $data['speed'] ?? 0,
        'fix'    => $fix,
        'sat'    => $data['sat'] ?? null,
        'useSat' => $data['useSat'] ?? null,
        'hdop'   => $data['hdop'] ?? null,
    ];
    $cache['lastUpdate'] = $now;
    writeCache($cache);
}

/** 解析并处理一条设备上行文本（原 bridge $handleMessage 逻辑） */
function ingestText(string $txt): void
{
    $txt = trim($txt);
    if ($txt === '') return;
    $parsed = parseTextMessage($txt);
    $type   = $parsed['type'];
    $data   = $parsed['data'];
    $now = time() * 1000;

    if ($type === 'bms' && $data !== null) {
        ingestLog("BMS 帧: " . ($data['totalVoltage'] ?? '?') . "V "
            . ($data['current'] ?? '?') . "A SOC=" . ($data['soc'] ?? '?') . "%");
        handleBMSData($data);
    } elseif ($type === 'gps') {
        if (!empty($data['fix'])) {
            ingestLog("GPS: {$data['lat']},{$data['lon']} speed={$data['speed']}km/h");
            handleGPSData($data, 'gps');
        } else {
            ingestLog("GPS 未定位");
            // 未定位也写入缓存，网页才能显示「未定位」而非过期位置
            $cache = readCache();
            $cache['gps'] = [
                'ts' => $now,
                'lon' => null, 'lat' => null,
                'speed' => 0, 'fix' => false,
                'sat' => null, 'useSat' => null, 'hdop' => null,
            ];
            $cache['lastUpdate'] = $now;
            writeCache($cache);
        }
    } elseif ($type === 'jzdw' && $data !== null) {
        $data['fix'] = 1;
        $data['speed'] = 0;
        handleGPSData($data, 'jzdw');
    } elseif ($type === 'blestatus' && $data !== null) {
        $cache = readCache();
        $cache['status'] = array_merge($cache['status'] ?? [], [
            'bleState'    => $data['state'],
            'bleMac'      => $data['mac'],
            'bleConnected'=> $data['connected'],
        ]);
        $cache['lastUpdate'] = $now;
        writeCache($cache);
        ingestLog("蓝牙状态: {$data['state']} ({$data['mac']})");
    } elseif ($type === 'csq' && $data !== null) {
        $cache = readCache();
        $cache['status'] = array_merge($cache['status'] ?? [], ['csq' => $data['csq']]);
        $cache['lastUpdate'] = $now;
        writeCache($cache);
    } else {
        ingestLog("忽略消息: {$txt}");
    }

    // 设备数据上报时同步检测在线状态（网页关闭后心跳停止，由设备数据触发下线切换）
    checkAndSendMode();
    // 周期性清理过期数据（内部节流，每 6 小时最多一次）
    cleanOldData();
}

/** 通过 EMQX REST API 发布下行指令（替代 bridge 的 MQTT publish） */
/**
 * MQTT 3.1.1 直连发布 (纯 PHP, 无第三方依赖)
 * 用途: 下行 mode 指令 —— 用 EMQX「客户端认证」账号直连发布到 controlTopic,
 *       绕开 Serverless REST API 密钥 403 (权限/白名单) 问题。
 * @return array [ok(bool), msg(string)]
 */
function encodeRemainingLength(int $len): string
{
    $out = '';
    do {
        $b = $len % 128;
        $len = intdiv($len, 128);
        if ($len > 0) $b |= 0x80;
        $out .= chr($b);
    } while ($len > 0);
    return $out;
}

function mqttPublishDirect(string $host, int $port, string $user, string $pass, string $topic, string $payload): array
{
    // EMQX Cloud Serverless 仅开放 TLS 端口(8883), 使用 tls:// 加密连接;
    // 关闭证书校验与 REST API 一致 (Let's Encrypt 公共证书, 部分虚拟主机缺 CA 包)
    $ctx = stream_context_create(['ssl' => [
        'verify_peer'       => false,
        'verify_peer_name'  => false,
        'allow_self_signed' => true,
    ]]);
    $fp = @stream_socket_client(
        "tls://{$host}:{$port}", $errno, $errstr, 8,
        STREAM_CLIENT_CONNECT, $ctx
    );
    if (!$fp) return [false, "TLS 连接失败: {$errstr} ({$errno})"];
    stream_set_timeout($fp, 8);  // 读写超时, 避免 fread 阻塞等满 default_socket_timeout

    $clientId = 'jk_srv_' . substr(md5(uniqid('', true)), 0, 8);

    // CONNECT: 协议名 MQTT / 级别 4 / 标志 0xC2 = clean session=true + username + password;
    //   ★ 必须 clean session=true: 0xC0(false) 会创建持久会话, 断开后 EMQX 侧保留会话
    //   (Serverless 默认约 2 小时) 且保留期间持续计入连接分钟 → 短连接频繁下发会堆积会话,
    //   2 天吃掉 45 万连接分钟(平均 157 并发)。clean=true 断开立即注销, 不再堆积。
    //   keepalive 60s (发布后立即 DISCONNECT, 不影响)
    $varHeader = "\x00\x04MQTT" . "\x04" . chr(0xC2) . pack('n', 60);
    $payloadC  = pack('n', strlen($clientId)) . $clientId
               . pack('n', strlen($user)) . $user
               . pack('n', strlen($pass)) . $pass;
    $body   = $varHeader . $payloadC;
    $packet = "\x10" . encodeRemainingLength(strlen($body)) . $body;
    fwrite($fp, $packet);

    // CONNACK: 4 字节, 返回码在第 4 字节 (0=成功)
    $ack = fread($fp, 4);
    if (strlen($ack) < 4 || $ack[0] !== "\x20") { fclose($fp); return [false, 'CONNACK 异常: ' . bin2hex($ack)]; }
    if (ord($ack[3]) !== 0) {
        fclose($fp);
        return [false, '连接被拒, 返回码=' . ord($ack[3]) . ' (1=协议错 2=标识被拒 3=服务器不可用 4=账号密码错 5=未授权)'];
    }

    // PUBLISH (QoS 0)
    $pubBody = pack('n', strlen($topic)) . $topic . $payload;
    $packet  = "\x30" . encodeRemainingLength(strlen($pubBody)) . $pubBody;
    fwrite($fp, $packet);

    // DISCONNECT
    fwrite($fp, "\xE0\x00");
    fclose($fp);
    return [true, "已发布到 {$topic}"];
}

function sendModeCommand(string $cmd): void
{
    $cfg = require __DIR__ . '/config.php';
    $m = $cfg['mqtt'] ?? [];

    // ★ 方式1 (v2.5): MQTT 直连发布 —— 用「客户端认证」账号, 绕开 REST API 403
    if (!empty($m['host']) && !empty($m['username']) && !empty($m['password'])) {
        $res = mqttPublishDirect(
            $m['host'],
            (int)($m['port'] ?? 1883),
            $m['username'],
            $m['password'],
            $m['controlTopic'] ?? 'R',
            $cmd
        );
        if ($res[0]) {
            ingestLog("mode 下发成功(MQTT直发): {$cmd}");
            return;
        }
        ingestLog("⚠ mode MQTT直发失败: {$res[1]}");
    } else {
        ingestLog("⚠ mqtt 配置不完整, 跳过下行: {$cmd}");
    }
}

/**
 * 在线检测 + 双模式下发（网页 heartbeat 时调用；替代 bridge 的 loop 检测）
 * 在线数 0→1 发 mode,realtime；1→0 发 mode,track；设置变化重下发间隔
 * 注意：HTTP 请求间无状态，用 app_settings 持久化 currentMode/hash（替代常驻进程的 static 变量）
 */
function checkAndSendMode(): void
{
    $cfg = require __DIR__ . '/config.php';

    $settings = getSettings();

    // 统计在线客户端
    $clients = readClients();
    $now = time() * 1000;
    $timeout = $cfg['onlineTimeout'] * 1000;
    $online = 0;
    // v2.19: 只有"停留在 BMS 页"的在线客户端才触发 realtime 高频;
    //   停留在其他页(或全部离线)按 track —— BMS 充/放电 30s、停放停报; GPS 静止省流。
    $bmsPageOnline = 0;
    foreach ($clients as $uid => $c) {
        $last = is_array($c) ? (int)($c['t'] ?? 0) : (int)$c;
        $page = is_array($c) ? (string)($c['page'] ?? 'gps') : 'gps';
        if ($now - $last < $timeout) {
            $online++;
            if ($page === 'bms') $bmsPageOnline++;
        }
    }

    // 持久化状态（跨请求共享）
    $currentMode = getAppSetting('currentMode', 'track');
    $lastSettingsHash = getAppSetting('lastSettingsHash', '');

    // 下发模式指令（带 BMS/GPS 间隔参数，ms）
    $sendModeCmd = function ($mode) use ($cfg, $settings) {
        // v2.8/v2.19: track 模式 bmsMs 传 0 —— 固件按电流判断: 充/放电(|I|>=0.3A) 30s 上报(续航学习), 停放停报
        $bmsMs = (int)(($mode === 'realtime' ? $settings['realtimeBmsSec'] : 0) * 1000);
        $gpsMs = (int)(($mode === 'realtime' ? $settings['realtimeGpsSec'] : 0) * 1000);
        sendModeCommand("mode,{$mode},{$bmsMs},{$gpsMs}");
    };

    // 设置变化 → 立即重下发当前模式的间隔
    $hash = md5(json_encode($settings));
    if ($hash !== $lastSettingsHash) {
        setAppSetting('lastSettingsHash', $hash);
        $sendModeCmd($currentMode);
    }

    // 状态变化 → 切换模式（只在变化时下发一次，避免每次心跳重复发）
    // v2.19: 触发 realtime 的条件 = 有客户端停留在 BMS 页（不只是"网页在线"）
    if ($bmsPageOnline > 0 && $currentMode !== 'realtime') {
        $currentMode = 'realtime';
        setAppSetting('currentMode', 'realtime');
        $sendModeCmd('realtime');
        ingestLog("BMS 页在线({$bmsPageOnline}个) → realtime");
    } elseif ($bmsPageOnline === 0 && $currentMode !== 'track') {
        $currentMode = 'track';
        setAppSetting('currentMode', 'track');
        $sendModeCmd('track');
        ingestLog("无 BMS 页在线 → track");
    }

    // ★ 自愈 (v2.4, 间隔可配置): 周期强制下行 —— 每 modePingInterval 秒重新下发当前 mode 指令,
    //   让 ESP32 感知 MQTT 下行链路存活; 若 MQTT 静默断开, ESP32 12 分钟收不到下行 → 自动重启。
    //   v2.18: 间隔 30s→120s (config.modePingInterval), 减少短连接次数 (每次至少计费 1 连接分钟)。
    $lastPing = (int)getAppSetting('lastModePing', '0');
    $pingInterval = (int)($cfg['modePingInterval'] ?? 120);
    if ($pingInterval > 0 && time() - $lastPing >= $pingInterval) {
        setAppSetting('lastModePing', (string)time());
        $sendModeCmd($currentMode);
    }

    // 统一写缓存（必须在模式切换之后，保证缓存反映最新状态）
    $cache = readCache();
    $cache['online'] = $online;
    $cache['mode'] = $currentMode;
    writeCache($cache);
}
