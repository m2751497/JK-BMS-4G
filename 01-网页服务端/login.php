<?php
define('JK_INCLUDED', true); // 标记为入口: 允许 require 内部文件
/**
 * 登录页
 */
require_once __DIR__ . '/auth.php';

if (authCheck()) {
    header('Location: index.php');
    exit;
}

$error = '';
if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    if (authIsLocked()) {
        $error = '失败次数过多，请 5 分钟后再试';
    } else {
        $user = trim($_POST['username'] ?? '');
        $pass = $_POST['password'] ?? '';
        if (authLogin($user, $pass)) {
            authResetFailures();
            header('Location: index.php');
            exit;
        }
        authRecordFailure();
        $error = '账号或密码错误';
    }
}
?>
<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1.0,user-scalable=no">
<title>JK BMS 4G 监控 · 登录</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
html,body{width:100%;height:100%;background:#0b1016;color:#dbe7f3;font-family:sans-serif;display:flex;align-items:center;justify-content:center}
.login-box{width:320px;background:#131b24;border:1px solid #223140;border-radius:14px;padding:32px 28px;box-shadow:0 8px 30px rgba(0,0,0,.5)}
h1{font-size:20px;color:#00cfff;text-align:center;margin-bottom:6px}
.sub{font-size:12px;color:#6f8399;text-align:center;margin-bottom:24px}
label{display:block;font-size:13px;color:#6f8399;margin:14px 0 6px}
input{width:100%;background:#0b1016;border:1px solid #2a3a4a;border-radius:8px;padding:10px 12px;color:#dbe7f3;font-size:14px;outline:none}
input:focus{border-color:#00cfff}
button{width:100%;margin-top:22px;background:#00cfff;border:none;border-radius:8px;padding:11px;font-size:15px;font-weight:bold;color:#0b1016;cursor:pointer}
button:hover{background:#33d9ff}
.err{color:#ff4d67;font-size:13px;text-align:center;margin-top:14px}
</style>
</head>
<body>
  <div class="login-box">
    <h1>🔋 JK BMS 4G 监控</h1>
    <div class="sub">请输入账号密码登录</div>
    <form method="post" autocomplete="off">
      <label>账号</label>
      <input type="text" name="username" required autofocus>
      <label>密码</label>
      <input type="password" name="password" required>
      <button type="submit">登 录</button>
      <?php if ($error): ?><div class="err"><?= htmlspecialchars($error) ?></div><?php endif; ?>
    </form>
  </div>
</body>
</html>
