<?php
// 防直接访问: 仅允许通过入口文件 require 加载
if (!defined('JK_INCLUDED')) { http_response_code(403); exit('forbidden'); }
/**
 * 网页账号登录鉴权（简单 session 方案）
 *  - config.php 的 auth 开启后，访问 index.php / api.php 需先登录
 *  - 账号密码优先读取设置表（网页「设置」页可在线修改），未设置时用 config.php 默认值
 *  - 登录信息存 session（7 天有效），前端 fetch 同域自动携带 cookie
 */

require_once __DIR__ . '/db.php';

/** 启动会话（必须在任何输出前调用） */
function authStart(): void
{
    if (session_status() === PHP_SESSION_NONE) {
        // Secure: HTTPS 下自动启用 (虚拟主机开 SSL 后 cookie 不再明文传输)
        $secure = (!empty($_SERVER['HTTPS']) && $_SERVER['HTTPS'] !== 'off')
               || (($_SERVER['SERVER_PORT'] ?? '') == 443);
        session_set_cookie_params([
            'lifetime' => 7 * 24 * 3600,
            'path'     => '/',
            'secure'   => $secure,
            'httponly' => true,
            'samesite' => 'Lax',
        ]);
        session_start();
    }
}

/** 读取当前生效的登录账号密码（设置表优先，其次 config.php） */
function authCreds(): array
{
    $auth = (require __DIR__ . '/config.php')['auth'];
    return [
        'username' => getAppSetting('authUsername', $auth['username']),
        'password' => getAppSetting('authPassword', $auth['password']),
    ];
}

/** 检查是否已登录（auth 关闭时直接放行） */
function authCheck(): bool
{
    $auth = (require __DIR__ . '/config.php')['auth'];
    if (empty($auth['enabled'])) return true;
    authStart();
    return !empty($_SESSION['bms_logged']);
}

/** 校验账号密码（不写入登录态）；密码兼容：新存 bcrypt 哈希 / 旧明文直接比较 */
function authVerify(string $user, string $pass): bool
{
    $creds = authCreds();
    if (!hash_equals($creds['username'], $user)) return false;
    $stored = $creds['password'];
    // bcrypt 哈希以 $2y$ 开头（PASSWORD_DEFAULT 在 PHP≥7.4 即 bcrypt）
    if (strlen($stored) >= 4 && substr($stored, 0, 4) === '$2y$') {
        return password_verify($pass, $stored);
    }
    return hash_equals($stored, $pass);
}

/** 校验账号密码，成功则写入登录态 */
function authLogin(string $user, string $pass): bool
{
    if (authVerify($user, $pass)) {
        authStart();
        $_SESSION['bms_logged'] = true;
        session_regenerate_id(true);
        return true;
    }
    return false;
}

/** 登出 */
function authLogout(): void
{
    authStart();
    $_SESSION = [];
    session_destroy();
}

/* ==================== 登录失败限速（防公网爆破） ==================== */
const LOGIN_MAX_FAILS    = 5;   // 5 分钟内最多失败 5 次
const LOGIN_LOCK_SECONDS = 300; // 锁定 5 分钟

/**
 * 限速按客户端 IP 计数 (而非全局): 防止攻击者故意输错密码,
 * 把管理员自己的全局计数刷满而锁死管理员登录。
 */
function loginFailKey(): string
{
    return 'loginFail_' . md5($_SERVER['REMOTE_ADDR'] ?? 'unknown');
}

/** 是否处于锁定状态（锁定窗口已过则自动复位） */
function authIsLocked(): bool
{
    $fails   = (int)getAppSetting(loginFailKey(), '0');
    $firstTs = (int)getAppSetting(loginFailKey() . 'Ts', '0');
    if ($fails >= LOGIN_MAX_FAILS) {
        if (time() - $firstTs < LOGIN_LOCK_SECONDS) return true;
        setAppSetting(loginFailKey(), '0');      // 锁定过期自动复位
        setAppSetting(loginFailKey() . 'Ts', '0');
    }
    return false;
}

/** 记录一次登录失败 */
function authRecordFailure(): void
{
    $fails   = (int)getAppSetting(loginFailKey(), '0');
    $firstTs = (int)getAppSetting(loginFailKey() . 'Ts', '0');
    if ($fails === 0) $firstTs = time();
    setAppSetting(loginFailKey(), (string)($fails + 1));
    setAppSetting(loginFailKey() . 'Ts', (string)$firstTs);
}

/** 登录成功清空失败计数 */
function authResetFailures(): void
{
    setAppSetting(loginFailKey(), '0');
    setAppSetting(loginFailKey() . 'Ts', '0');
}
