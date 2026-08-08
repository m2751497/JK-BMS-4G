<?php
define('JK_INCLUDED', true); // 标记为入口: 允许 require 内部文件
/** 登出 */
require_once __DIR__ . '/auth.php';
authLogout();
header('Location: login.php');
exit;
