/*
 * PSRAM 查看工具 (ESP32-S3) —— 独立小工具, 与主固件无关
 *
 * 用途: 判断你的板子 PSRAM 是 QSPI 还是 OPI、编译模式是否匹配、是否生效。
 *
 * 用法:
 *   1. 本文件所在文件夹打开(psram-check/psram-check.ino), Arduino IDE 编译
 *   2. Tools → Board: ESP32S3 Dev Module
 *   3. Tools → USB CDC On Boot: Enabled (原生 USB 口才能看串口日志)
 *   4. Tools → PSRAM: 先试 QSPI PSRAM 烧录看日志; 若 Size=0 再换 OPI PSRAM 烧录
 *   5. 串口监视器 115200, 看输出:
 *        PSRAM Size = 2097152 B (2MB) → 生效 ✓ (该 PSRAM 模式就是对的)
 *        PSRAM Size = 0 B            → 未生效 ✗ (换另一种 PSRAM 模式再试)
 *   6. 查完后烧回主固件即可
 *
 * 不需要任何第三方库。
 */

#include <Arduino.h>

void setup() {
  Serial.begin(115200);
  delay(800);

  Serial.println();
  Serial.println("======================================");
  Serial.println(" PSRAM 查看工具 (ESP32-S3)");
  Serial.println("======================================");

  size_t psramSize = ESP.getPsramSize();
  size_t psramFree = ESP.getFreePsram();

  Serial.printf("PSRAM Size : %u B (%u KB)\n", (uint32_t)psramSize, (uint32_t)(psramSize / 1024));
  Serial.printf("PSRAM Free : %u B (%u KB)\n", (uint32_t)psramFree, (uint32_t)(psramFree / 1024));
  Serial.printf("Heap 总    : %u B (%u KB)\n", (uint32_t)ESP.getHeapSize(), (uint32_t)(ESP.getHeapSize() / 1024));
  Serial.printf("Heap 可用  : %u B (%u KB)\n", (uint32_t)ESP.getFreeHeap(), (uint32_t)(ESP.getFreeHeap() / 1024));

  if (psramSize > 0) {
    Serial.println();
    Serial.println("→ PSRAM 生效 ✓ (当前编译的 PSRAM 模式与硬件匹配)");
    Serial.println("  若 Size=2097152(2MB), 主固件也用这个 PSRAM 模式编译即可");
  } else {
    Serial.println();
    Serial.println("→ PSRAM 未生效 ✗ (编译 PSRAM 模式与硬件不符)");
    Serial.println("  请换另一种 PSRAM 模式重编再烧: QSPI PSRAM ↔ OPI PSRAM");
  }
  Serial.println();
  Serial.println("查看完毕, 可烧回主固件。");
}

void loop() {
  delay(10000);
}
