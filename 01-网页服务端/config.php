<?php
// 防直接访问: 仅允许通过入口文件 (index.php/api.php/login.php/logout.php) require 加载
if (!defined('JK_INCLUDED')) { http_response_code(403); exit('forbidden'); }
/**
 * 全局配置（WebHook 方案：无常驻进程）
 * 部署到虚拟主机/宝塔后，数据由 EMQX 规则引擎自动转发到：
 *   http://你的域名/api.php?action=ingest （请求头带 X-Ingest-Token）
 * 下行 mode 指令通过 MQTT 直连（EMQX「客户端认证」账号，TLS 8883）发布到 controlTopic，无需任何常驻进程。
 */

return [
    // ==================== MQTT 主题（EMQX） ====================
    // WebHook 方案仅用主题名：数据上行由 EMQX 规则自动转发，下行 mode 指令发到 controlTopic
    'mqtt' => [
        'topic'        => 'T',                  // 设备 → EMQX 上行主题
        'controlTopic' => 'R',                  // 服务端 → 设备 下行主题
        // ★ 下行发布凭据 (v2.5 起): 用 EMQX「客户端认证」账号直连发布 (TLS 8883),
        //   ⚠ 本仓库为公开模板: 部署时请填真实值, 勿提交真实账号密码!
        'host'     => '你的EMQX地址.emqxsl.cn', // 与 EMQX 连接地址相同
        'port'     => 8883,                     // MQTT over TLS 端口 (Serverless 用 8883)
        'username' => '你的MQTT账号',           // “访问控制”→“客户端认证”账号
        'password' => '你的MQTT密码',           // “访问控制”→“客户端认证”密码
    ],

    // ==================== 数据与缓存 ====================
    'db'          => __DIR__ . '/data/bms.db',       // SQLite 数据库文件
    'cacheFile'   => __DIR__ . '/data/latest.json', // 最新数据缓存（桥接进程写，网页读）
    'clientsFile' => __DIR__ . '/data/clients.json',// 网页客户端心跳表
    'ingestLog'   => __DIR__ . '/data/ingest.log',  // WebHook 接收日志（替代 bridge 终端日志）

    // ==================== WebHook 接收（无常驻进程方案） ====================
    // 启用后：EMQX 规则引擎把 T 主题消息转发到 http://你的域名/api.php?action=ingest
    // 请求头需带 X-Ingest-Token: <token>（防伪造）。部署到虚拟主机时无需任何常驻进程。
    'webhook' => [
        'enabled' => true,
        'token'   => '改成你的随机长串',  // ⚠ 部署后请改成随机长字符串
    ],

    // ==================== 业务参数 ====================
    'onlineTimeout'    => 25,       // 网页心跳超时（秒），超过判定离线
    'modeCheckInterval'=> 5,        // 在线状态检测间隔（秒）
    'modePingInterval' => 120,      // ★ 下行自愈间隔（秒）: 周期强制重发 mode 指令给 ESP32 保活。
                                    //   v2.18: 30→120 秒 —— MQTT 短连接每次至少计费 1 连接分钟,
                                    //   频繁下发会消耗 Serverless 免费额度 (2 天曾吃掉 45 万分钟)。
                                    //   仍远小于 ESP32 的 12 分钟无下行自动重启阈值, 保活有效。
    'rangeFactorDefault'=> 2.0,     // 里程基数默认值（km/Ah）

    // ==================== 网页登录（账号密码） ====================
    // 部署公网后务必修改默认密码！enabled=false 可关闭登录（仅本地调试用）
    'auth' => [
        'enabled'  => true,
        'username' => 'admin',
        'password' => '改成你的密码',
    ],

    // 可调设置（网页「设置」页可改，存 SQLite；设备上报间隔由桥接进程下发到 ESP32）
    'settings' => [
        'webPollSec'        => 2,    // 网页轮询间隔（秒）
        'realtimeBmsSec'    => 2,    // 实时模式 BMS 上报间隔（秒，网页开+充/放电时）
        'realtimeGpsSec'    => 2,    // 实时模式 GPS 上报间隔（秒，移动时；GPS 帧小，调快几乎不耗流量）
        'dataRetentionDays' => 60,   // 数据保留天数（超过自动清理，0=不清理）
    ],
];
