#include "Arduino.h"
#include "../../globals.h"
#include "../../types.h"
#include "battery.h"
#include "draw.h"
#include "../../config.h"
#include <math.h>

#if ARDUINO_USB_CDC_ON_BOOT
#include "tusb.h"
#endif

static bool palaAdcReady = false;

void batteryInit() {
  if (palaAdcReady) return;
  pinMode(BAT_ADC_PIN, INPUT);
  analogSetPinAttenuation(BAT_ADC_PIN, ADC_11db);
  analogReadMilliVolts(BAT_ADC_PIN);
  palaAdcReady = true;
}

static float readBatteryVoltageSamples(int samples) {
  if (!palaAdcReady) batteryInit();
  if (samples < 1) samples = 1;
  uint32_t sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogReadMilliVolts(BAT_ADC_PIN);
    delay(2);
  }
  float mv = (float)sum / (float)samples;
  return (mv / 1000.0f) * 2.0f;
}

float readBatteryVoltage(int samples) {
  return readBatteryVoltageSamples(samples);
}

int batteryPercentFromVoltage(float v) {
  if (v >= 4.35f) return 100;
  if (v <= 3.20f) return 0;
  // This board's divider reads ~0.1 V low vs cell voltage at rest.
  if (v >= 4.08f) return 100;
  const float volts[] = {3.20f, 3.40f, 3.70f, 3.90f, 4.08f};
  const int   pct[]   = {0,     25,    50,    75,    100};
  for (int i = 1; i < 5; i++) {
    if (v <= volts[i]) {
      float t = (v - volts[i-1]) / (volts[i] - volts[i-1]);
      int p = pct[i-1] + (int)((pct[i] - pct[i-1]) * t + 0.5f);
      p = ((p + 2) / 5) * 5;
      return constrain(p, 0, 100);
    }
  }
  return 100;
}

int readBatteryPercent(int samples) {
  float v = readBatteryVoltageSamples(samples);
  if (v <= 0.1f) return -1;
  return batteryPercentFromVoltage(v);
}

static float gChargeLastV = 0;
static uint32_t gChargeLastMs = 0;
static uint32_t gChargeHoldUntilMs = 0;

// USB enumerated by host (cable to a computer). Unlike (bool)Serial — which is
// only true when DTR+RTS are asserted (Serial Monitor open) — tud_mounted() is
// true as soon as the host configures the device. See Espressif IDF #7747 and
// arduino-esp32 USBCDC.cpp (connected <= dtr && rts).
// Note: a dumb wall charger with no data lines never enumerates; use voltage.
static bool isUsbMounted() {
#if ARDUINO_USB_CDC_ON_BOOT
  return tud_mounted();
#else
  return false;
#endif
}

bool isBatteryChargingAt(float v) {
  if (v <= 0.1f) return false;

  bool charging = false;

  if (isUsbMounted() && v > 3.45f) charging = true;
  // ADC reads ~0.1 V low; ETA6098 CV is 4.16–4.24 V cell ≈ 4.06–4.14 on ADC.
  if (!charging && v >= 4.10f) charging = true;

  uint32_t now = millis();
  if (!charging) {
    if (gChargeLastMs != 0 && (now - gChargeLastMs) >= 3000) {
      if (v > gChargeLastV + 0.012f) charging = true;
      gChargeLastV = v;
      gChargeLastMs = now;
    } else if (gChargeLastMs == 0) {
      gChargeLastV = v;
      gChargeLastMs = now;
    }
  } else {
    gChargeLastV = v;
    gChargeLastMs = now;
  }

  // Hold bolt on briefly so e-paper refresh / LED pin fighting don't flicker it off.
  if (charging) gChargeHoldUntilMs = now + 15000UL;
  return charging || (int32_t)(gChargeHoldUntilMs - now) > 0;
}

bool isBatteryCharging() {
  return isBatteryChargingAt(readBatteryVoltage());
}

void drawThickArcDot(int cx, int cy, int r, int deg, int thickness, uint8_t color) {
  float a = ((float)deg - 90.0f) * PI / 180.0f;
  int x = cx + (int)roundf(cosf(a) * r);
  int y = cy + (int)roundf(sinf(a) * r);
  if (thickness <= 1) px(x, y, color);
  else fillCircle(x, y, thickness / 2, color);
}

void drawBatteryRing(int percent) {
  const int cx = 100, cy = 100, r = 82;
  strokeCircle(cx, cy, r, 1, BLACK);
  if (percent < 0) return;
  percent = constrain(percent, 0, 100);
  int endDeg = (360 * percent) / 100;
  for (int deg = 0; deg <= endDeg; deg += 2)
    drawThickArcDot(cx, cy, r, deg, 3, BLACK);
}
