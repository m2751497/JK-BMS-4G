<?php
// 防直接访问 data/ 目录(数据库/缓存/日志)
// Apache 由 .htaccess 拦截;Nginx/宝塔需在站点配置加:
//   location ~ ^/data/ { deny all; }
// 本守卫用于未配 nginx 规则时,直接访问 data/ 或 data/ 下无索引文件的兜底
http_response_code(403);
exit('forbidden');
