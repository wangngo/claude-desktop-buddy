#pragma once
// Hardware abstraction: maps the M5StickCPlus-era API this firmware was
// written against onto M5Unified, so the same source builds for
// M5StickC Plus (AXP192) and M5StickS3 (ESP32-S3 + M5PM1 PMIC).
#include <M5Unified.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

// ---- Display types (TFT_eSPI names -> LovyanGFX/M5GFX) ----
using TFT_eSPI    = lgfx::LovyanGFX;
using TFT_eSprite = M5Canvas;

// ---- RTC structs with the old field names ----
struct RTC_TimeTypeDef { uint8_t Hours, Minutes, Seconds; };
struct RTC_DateTypeDef { uint8_t WeekDay, Month, Date; uint16_t Year; };

struct HalRtc {
  void SetTime(const RTC_TimeTypeDef* t) {
    M5.Rtc.setTime(m5::rtc_time_t(t->Hours, t->Minutes, t->Seconds));
  }
  void SetDate(const RTC_DateTypeDef* d) {
    M5.Rtc.setDate(m5::rtc_date_t(d->Year, d->Month, d->Date, d->WeekDay));
  }
  void GetTime(RTC_TimeTypeDef* t) {
    auto r = M5.Rtc.getTime();
    t->Hours = r.hours; t->Minutes = r.minutes; t->Seconds = r.seconds;
  }
  void GetDate(RTC_DateTypeDef* d) {
    auto r = M5.Rtc.getDate();
    d->Year = r.year; d->Month = r.month; d->Date = r.date; d->WeekDay = r.weekDay;
  }
};

// ---- Power (AXP192-style calls -> M5.Power / M5.Display) ----
struct HalPower {
  // AXP192 ScreenBreath took 0..100.
  void ScreenBreath(int pct) {
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    M5.Display.setBrightness((uint8_t)(pct * 255 / 100));
  }
  // LDO2 fed the LCD backlight on StickC Plus: on/off == wake/sleep panel.
  void SetLDO2(bool on) { if (on) M5.Display.wakeup(); else M5.Display.sleep(); }
  void PowerOff() { M5.Power.powerOff(); }
  // Deep sleep, wake on BtnA (active-low). Chip reboots into setup().
  // We arm the wakeup ourselves and pass touch_wakeup=false so M5Unified
  // doesn't apply its own pin config (it enables a pull-down on the wake pin).
  void DeepSleepOnBtnA() {
    if (M5.getBoard() == m5::board_t::board_M5StickCPlus) {
#if SOC_PM_SUPPORT_EXT0_WAKEUP
      esp_sleep_enable_ext0_wakeup(GPIO_NUM_37, 0);   // BtnA, external pull-up
#endif
    } else {                                           // StickS3: BtnA = GPIO11
#if SOC_PM_SUPPORT_EXT1_WAKEUP
      const gpio_num_t pin = GPIO_NUM_11;
      esp_sleep_enable_ext1_wakeup(1ULL << pin, ESP_EXT1_WAKEUP_ANY_LOW);
      rtc_gpio_pullup_en(pin);
      rtc_gpio_pulldown_dis(pin);
#endif
    }
    M5.Power.deepSleep(M5.Power.sleep_no_timer, false);
  }
  float GetBatVoltage()  { return M5.Power.getBatteryVoltage() / 1000.0f; }
  float GetBatCurrent()  { return (float)M5.Power.getBatteryCurrent(); }
  float GetVBusVoltage() {
    int16_t mv = M5.Power.getVBUSVoltage();
    if (mv < 0) {  // PMIC can't measure VBUS: infer from charge state
      return M5.Power.isCharging() == m5::Power_Class::is_charging ? 5.0f : 0.0f;
    }
    return mv / 1000.0f;
  }
  float GetTempInAXP192() { return temperatureRead(); }  // ESP32 die temp
  // 0x02 == short click, matching AXP192 GetBtnPress()
  uint8_t GetBtnPress() { return M5.BtnPWR.wasClicked() ? 0x02 : 0; }
};

struct HalBeep {
  void begin() {}
  void update() {}
  void tone(uint16_t f, uint16_t d) { M5.Speaker.tone(f, d); }
};

extern HalRtc   HRtc;
extern HalPower HAxp;
extern HalBeep  HBeep;

// ---- Status LED ----
// StickC Plus: red LED on GPIO10, active-low. StickS3 has no user LED
// (GPIO10 is on the expansion port), so the LED is a no-op there.
inline void halLedInit() {
  if (M5.getBoard() == m5::board_t::board_M5StickCPlus) {
    pinMode(10, OUTPUT); digitalWrite(10, HIGH);
  }
}
inline void halLed(bool on) {
  if (M5.getBoard() == m5::board_t::board_M5StickCPlus) digitalWrite(10, on ? LOW : HIGH);
}
