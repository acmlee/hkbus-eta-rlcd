/*
 * rtc_wrap.cpp — C bridge between the C firmware (main.c) and the
 * vendored Waveshare PCF85063 driver (C++). Intentionally thin: all
 * register/BCD logic lives in the vendored driver, unchanged.
 *
 * The only deviations from the Waveshare example are:
 *  - Rtc_Setup() returns bool so a missing/dead chip degrades gracefully
 *  - the I2C bus global moved here (the example owned it in user_app.cpp)
 */
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <esp_log.h>
#include "rtc_pcf85063.h"
#include "i2c_bsp.h"
#include "i2c_equipment.h"

#define TAG "rtc"

/* Board I2C bus: SDA = GPIO13, SCL = GPIO14, I2C port 0 — matches the
 * Waveshare 04_I2C_PCF85063 example (I2cMasterBus(scl_pin, sda_pin, port)).
 * The bus is shared with the SHTC3 / audio codec, but this firmware only
 * drives the RTC, so there is no contention. */
static I2cMasterBus s_i2c_bus(14, 13, 0);

static bool s_rtc_ready = false;

/* Minimum epoch that may be stored into / restored from the RTC.
 * 1700000000 = 2023-11-14, same threshold as EPOCH_SYNC_THRESHOLD in
 * main.c. Guards against writing the pre-sync epoch-0 clock and against
 * restoring a garbage value. */
#define RTC_MIN_VALID_EPOCH 1700000000

/* PCF85063 BCD year 0x00 maps to 2000; anything before 2024 means the
 * RTC was never set (first boot) or the backup battery failed. */
#define RTC_MIN_VALID_YEAR  2024

/* Trust gate (docs/plan-rtc-pcf85063.md §11): an RTC year below this is
 * presumed severely unsynced (never recently synced, years of power-off,
 * or dead backup battery) and is NOT applied to the system clock. The
 * clock stays hidden (date/time/ETAs) until the first successful SNTP
 * sync. Distinct from RTC_MIN_VALID_YEAR, which is only a plausibility
 * floor for reads. */
#define RTC_MIN_TRUSTED_YEAR 2026

extern "C" bool rtc_pcf85063_init(void)
{
    if (s_rtc_ready) {
        return true;
    }
    /* 0x51 = PCF85063A slave address (PCF85063Constants.h) */
    s_rtc_ready = Rtc_Setup(&s_i2c_bus, 0x51);
    if (s_rtc_ready) {
        ESP_LOGI(TAG, "PCF85063 RTC ready (I2C0, SDA=13 SCL=14)");
    } else {
        ESP_LOGW(TAG, "PCF85063 init failed — RTC disabled, SNTP-only mode");
    }
    return s_rtc_ready;
}

extern "C" bool rtc_pcf85063_get_time(struct tm *out)
{
    if (!s_rtc_ready || !out) {
        return false;
    }
    rtcTimeStruct_t t;
    Rtc_GetTime(&t);
    /* The upstream getDateTime() leaves its buffer untouched on a failed
     * read, so the fields must always pass a plausibility check. */
    if (t.year < RTC_MIN_VALID_YEAR || t.year > 2099 ||
        t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 ||
        t.hour > 23 || t.minute > 59 || t.second > 59) {
        ESP_LOGW(TAG, "RTC read failed plausibility check (%04d-%02d-%02d %02d:%02d:%02d)",
                 t.year, t.month, t.day, t.hour, t.minute, t.second);
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->tm_year = t.year - 1900;
    out->tm_mon  = t.month - 1;
    out->tm_mday = t.day;
    out->tm_hour = t.hour;
    out->tm_min  = t.minute;
    out->tm_sec  = t.second;
    out->tm_isdst = 0;
    return true;
}

extern "C" bool rtc_pcf85063_set_time(const struct tm *in)
{
    if (!s_rtc_ready || !in) {
        return false;
    }
    Rtc_SetTime(in->tm_year + 1900, in->tm_mon + 1, in->tm_mday,
                in->tm_hour, in->tm_min, in->tm_sec);
    return true;
}

extern "C" void rtc_pcf85063_store_system_time(void)
{
    if (!s_rtc_ready) {
        return;
    }
    time_t now = time(NULL);
    struct tm info;
    if (now < RTC_MIN_VALID_EPOCH) {
        return;                     /* clock not synced yet — don't persist epoch-0 */
    }
    if (localtime_r(&now, &info) == NULL) {
        return;
    }
    rtc_pcf85063_set_time(&info);
    ESP_LOGI(TAG, "RTC updated from SNTP: %04d-%02d-%02d %02d:%02d:%02d HKT",
             info.tm_year + 1900, info.tm_mon + 1, info.tm_mday,
             info.tm_hour, info.tm_min, info.tm_sec);
}

extern "C" bool rtc_pcf85063_restore_system_time(void)
{
    if (!s_rtc_ready) {
        return false;
    }
    struct tm t;
    if (!rtc_pcf85063_get_time(&t)) {
        return false;
    }
    /* Trust gate (plan-rtc-pcf85063.md §11): a year below
     * RTC_MIN_TRUSTED_YEAR means the RTC was never synced recently.
     * Do not apply it — the clock stays untrusted (date/time/ETAs
     * hidden) until the first successful SNTP sync. */
    if ((t.tm_year + 1900) < RTC_MIN_TRUSTED_YEAR) {
        ESP_LOGW(TAG, "RTC year %d < %d — clock not trusted, hiding until first SNTP sync",
                 t.tm_year + 1900, RTC_MIN_TRUSTED_YEAR);
        return false;
    }
    /* mktime() interprets the tm as HKT wall time (TZ is already set). */
    time_t epoch = mktime(&t);
    if (epoch < RTC_MIN_VALID_EPOCH) {
        return false;
    }
    struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
    if (settimeofday(&tv, NULL) != 0) {
        ESP_LOGW(TAG, "settimeofday from RTC failed");
        return false;
    }
    ESP_LOGI(TAG, "System time restored from RTC: %04d-%02d-%02d %02d:%02d:%02d HKT",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return true;
}
