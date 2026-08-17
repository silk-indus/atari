#include <Arduino.h>
#include "rtc_boot.h"
#define RTC_BOOT_WORD_ADDR 0x50001FF0u
#define RTC_BOOT_ATARI_MAGIC 0x41544152u
void boot_atari_once(void) {
    volatile uint32_t *p=(volatile uint32_t*)RTC_BOOT_WORD_ADDR;
    *p=RTC_BOOT_ATARI_MAGIC;
    __asm__ __volatile__("memw");
    delay(20);
    ESP.restart();
}
