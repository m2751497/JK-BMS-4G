<?php
// ==================== 告警检测 + 充电循环记录 ====================
// ★融合参考 Node 版 (JK-BMS-4G-main/Node-RED/server.js):
//   - checkAlarms: 单体过压/欠压/压差/温度/SOC 阈值告警, 5 分钟同消息去重, 存 alarms 表
//   - checkChargeCycle: 充电周期记录 (start/end/SOC/Ah), 存 charge_cycles 表
// 依赖 db.php (JK_INCLUDED 守卫), 由 ingest.php 在 BMS 数据入库后调用。
// 电流符号与本项目一致: 充电 = 正电流 (current > 0.1)。

/** 告警检测: 阈值在 config.php 'alarms' 段, 可在线关闭 */
function checkAlarms(array $data): void
{
    $cfg = (require __DIR__ . '/config.php')['alarms'] ?? [];
    if (empty($cfg['enabled'])) return;

    // 单体过压 / 欠压
    foreach (($data['cellVoltages'] ?? []) as $idx => $v) {
        if ($v >= (float)$cfg['maxCellV']) {
            addAlarm('critical', sprintf('电芯 C%02d 过压: %.3fV', $idx + 1, $v));
        } elseif ($v > 0 && $v <= (float)$cfg['minCellV']) {
            addAlarm('critical', sprintf('电芯 C%02d 欠压: %.3fV', $idx + 1, $v));
        }
    }

    // 单体压差过大
    $delta = (float)($data['deltaCellV'] ?? 0);
    if ($delta >= (float)$cfg['deltaCellV']) {
        addAlarm('warning', sprintf('单体压差过大: %dmV', (int)round($delta * 1000)));
    }

    // 温度过高 (取 3 路温度最大值)
    $maxT = max((float)($data['temp1'] ?? 0), (float)($data['temp2'] ?? 0), (float)($data['mosTemp'] ?? 0));
    if ($maxT >= (float)$cfg['maxTemp']) {
        addAlarm('critical', sprintf('温度过高: %.1f℃', $maxT));
    }

    // SOC 过低
    $soc = (int)($data['soc'] ?? 100);
    if ($soc <= (int)$cfg['minSOC']) {
        addAlarm('warning', "SOC过低: {$soc}%");
    }
}

/** 写告警: 5 分钟同消息去重 */
function addAlarm(string $level, string $message): void
{
    $pdo = db();
    $st = $pdo->prepare('SELECT id FROM alarms WHERE ts > ? AND message = ?');
    $st->execute([time() * 1000 - 300000, $message]);
    if ($st->fetchColumn()) return;   // 5 分钟内同类告警不重复刷屏

    $pdo->prepare('INSERT INTO alarms (ts, level, message) VALUES (?, ?, ?)')
        ->execute([time() * 1000, $level, $message]);
    ingestLog("[告警][{$level}] {$message}");
}

/**
 * 充电循环记录 (★融合 Node 版 charge_cycles):
 *   充电开始 → 记 startTime/startSOC/startCap; 充电结束 → 写入一条 cycle (chargedAh = 结束容量 - 开始容量)
 *   状态存 app_settings 跨请求生效 (与里程学习同模式)。
 */
function checkChargeCycle(array $data): void
{
    $cfg = (require __DIR__ . '/config.php');
    if (empty($cfg['alarms']['enabled'])) return;   // 与告警同开关, 或单独加开关

    $current = (float)($data['current'] ?? 0);
    $isCharging = $current > 0.1;                    // 充电 = 正电流
    $soc     = (int)($data['soc'] ?? 0);
    $cap     = (float)($data['capRemain'] ?? 0);
    $now     = time() * 1000;

    $active   = getAppSetting('chgActive', 'false') === 'true';
    $startTs  = (int)getAppSetting('chgStartTs', '0');
    $startSoc = (int)getAppSetting('chgStartSoc', '0');
    $startCap = (float)getAppSetting('chgStartCap', '0');

    if ($isCharging && !$active) {
        // 充电开始: 记录起点
        setAppSetting('chgActive', 'true');
        setAppSetting('chgStartTs', (string)$now);
        setAppSetting('chgStartSoc', (string)$soc);
        setAppSetting('chgStartCap', (string)$cap);
    } elseif (!$isCharging && $active) {
        // 充电结束: 写入循环记录
        $chargedAh = round($cap - $startCap, 2);     // 充电增加的电量
        db()->prepare('INSERT INTO charge_cycles (startTime, endTime, startSOC, endSOC, startCap, endCap, chargedAh)
                       VALUES (?, ?, ?, ?, ?, ?, ?)')
            ->execute([$startTs, $now, $startSoc, $soc, $startCap, $cap, $chargedAh]);
        ingestLog("[充电循环] 完成: " . round($startCap, 2) . "→{$cap}Ah (充入 {$chargedAh}Ah, SOC {$startSoc}→{$soc}%)");
        setAppSetting('chgActive', 'false');
        setAppSetting('chgStartTs', '0');
        setAppSetting('chgStartSoc', '0');
        setAppSetting('chgStartCap', '0');
    }
}
